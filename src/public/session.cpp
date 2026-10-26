#include <video_transmission/video_transmission.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct Frame {
    vt_encoded_frame info{};
    std::vector<uint8_t> bytes;
    Frame(const vt_encoded_frame& f) : info(f), bytes(f.data, f.data + f.size) {
        info.data = bytes.data();
    }
    Frame(Frame&& f) noexcept : info(f.info), bytes(std::move(f.bytes)) {
        info.data = bytes.data();
    }
    Frame& operator=(Frame&& f) noexcept {
        info = f.info;
        bytes = std::move(f.bytes);
        info.data = bytes.data();
        return *this;
    }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};
struct Item {
    enum Kind { Encoded, Raw } kind;
    Clock::time_point time;
    size_t bytes;
    std::unique_ptr<Frame> encoded;
    vt_raw_frame raw{};
    Item(std::unique_ptr<Frame> f)
        : kind(Encoded), time(Clock::now()), bytes(f->bytes.size()), encoded(std::move(f)) {}
    Item(const vt_raw_frame& f, size_t n) : kind(Raw), time(Clock::now()), bytes(n), raw(f) {}
    Item(Item&& other) noexcept
        : kind(other.kind), time(other.time), bytes(other.bytes),
          encoded(std::move(other.encoded)), raw(other.raw) {
        other.raw.release = nullptr;
    }
    Item& operator=(Item&&) = delete;
    Item(const Item&) = delete;
    Item& operator=(const Item&) = delete;
    ~Item() {
        if (kind == Raw && raw.release) raw.release(raw.native_buffer);
    }
};
struct Incoming {
    enum Kind { Encoded, Raw, Event } kind;
    Clock::time_point time = Clock::now();
    std::unique_ptr<Frame> encoded;
    vt_raw_frame raw{};
    vt_event event{};
    size_t bytes = 0;
    Incoming(std::unique_ptr<Frame> f)
        : kind(Encoded), encoded(std::move(f)), bytes(encoded->bytes.size()) {}
    Incoming(const vt_raw_frame& f) : kind(Raw), raw(f) {}
    Incoming(vt_event_type type, vt_status status) : kind(Event) {
        event = {sizeof(vt_event), type, status};
    }
    Incoming(Incoming&& other) noexcept
        : kind(other.kind), time(other.time), encoded(std::move(other.encoded)), raw(other.raw),
          event(other.event), bytes(other.bytes) {
        other.raw.release = nullptr;
    }
    Incoming& operator=(Incoming&&) = delete;
    Incoming(const Incoming&) = delete;
    Incoming& operator=(const Incoming&) = delete;
    ~Incoming() {
        if (kind == Raw && raw.release) raw.release(raw.native_buffer);
    }
};

bool Valid(const vt_encoded_frame* f, size_t max_bytes) {
    return f && f->struct_size >= sizeof(vt_encoded_frame) && f->data && f->size &&
        f->size <= max_bytes && f->width && f->height && f->width <= 16384 &&
        f->height <= 16384;
}
bool Valid(const vt_raw_frame* f) {
    return f && f->struct_size >= sizeof(vt_raw_frame) && f->native_buffer && f->retain &&
        f->release && f->width && f->height && f->width <= 16384 && f->height <= 16384;
}
}  // namespace

struct vt_session {
    vt_session_config config;
    vt_callbacks callbacks;
    vt_transport_backend transport;
    vt_media_backend media{};
    vt_control_backend control{};
    bool has_media = false, has_control = false;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Item> outgoing;
    std::deque<Incoming> incoming;
    size_t outgoing_bytes = 0, incoming_bytes = 0;
    bool sending = false;
    size_t control_inflight = 0;
    std::thread worker, dispatcher;
    bool running = false, used = false;
    vt_event state{sizeof(vt_event), VT_EVENT_LINK_DOWN, VT_NOT_READY};

    void EventLocked(vt_event_type type, vt_status status) {
        state = {sizeof(vt_event), type, status};
        if (incoming.size() >= 32) {
            incoming_bytes -= incoming.front().bytes;
            incoming.pop_front();
        }
        incoming.emplace_back(type, status);
        cv.notify_all();
    }
    void Event(vt_event_type type, vt_status status) {
        std::lock_guard lock(mutex);
        EventLocked(type, status);
    }
    void Receive(const vt_encoded_frame* frame) {
        if (!Valid(frame, config.max_frame_bytes)) return;
        auto owned = std::make_unique<Frame>(*frame);
        std::lock_guard lock(mutex);
        if (!running) return;
        bool dropped = false;
        while (!incoming.empty() &&
               (incoming.size() >= config.max_frames ||
                incoming_bytes + owned->bytes.size() > config.max_bytes)) {
            incoming_bytes -= incoming.front().bytes;
            dropped = dropped || incoming.front().kind != Incoming::Event;
            incoming.pop_front();
        }
        incoming_bytes += owned->bytes.size();
        incoming.emplace_back(std::move(owned));
        if (dropped) EventLocked(VT_EVENT_FRAME_DROPPED, VT_QUEUE_FULL);
        cv.notify_all();
    }
    void ReceiveRaw(const vt_raw_frame* frame) {
        if (!Valid(frame)) return;
        frame->retain(frame->native_buffer);
        try {
            std::lock_guard lock(mutex);
            if (running) {
                if (incoming.size() >= config.max_frames) {
                    incoming_bytes -= incoming.front().bytes;
                    const bool dropped_frame = incoming.front().kind != Incoming::Event;
                    incoming.pop_front();
                    if (dropped_frame) EventLocked(VT_EVENT_FRAME_DROPPED, VT_QUEUE_FULL);
                }
                incoming.emplace_back(*frame);
                cv.notify_all();
            } else frame->release(frame->native_buffer);
        } catch (...) {
            frame->release(frame->native_buffer);
            throw;
        }
    }
    void SendLoop() {
        for (;;) {
            std::unique_ptr<Item> item;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return !running || !outgoing.empty(); });
                if (!running) break;
                item = std::make_unique<Item>(std::move(outgoing.front()));
                outgoing_bytes -= item->bytes;
                outgoing.pop_front();
                sending = true;
            }
            if (Clock::now() - item->time > std::chrono::milliseconds(config.max_age_ms)) {
                Event(VT_EVENT_FRAME_DROPPED, VT_NOT_READY);
                {
                    std::lock_guard lock(mutex);
                    sending = false;
                }
                cv.notify_all();
                continue;
            }
            vt_status rc = VT_UNSUPPORTED;
            if (item->kind == Item::Encoded)
                rc = transport.send(transport.context, &item->encoded->info);
            else if (has_media && media.encode)
                rc = media.encode(media.context, &item->raw);
            if (rc != VT_OK) Event(VT_EVENT_FRAME_DROPPED, rc);
            {
                std::lock_guard lock(mutex);
                sending = false;
            }
            cv.notify_all();
        }
    }
    void DispatchLoop() {
        for (;;) {
            std::unique_ptr<Incoming> item;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return !running || !incoming.empty(); });
                if (!running) break;
                item = std::make_unique<Incoming>(std::move(incoming.front()));
                incoming_bytes -= item->bytes;
                incoming.pop_front();
            }
            if (item->kind == Incoming::Event) {
                if (callbacks.on_event) callbacks.on_event(callbacks.user, &item->event);
            } else if (item->kind == Incoming::Encoded) {
                if (Clock::now() - item->time > std::chrono::milliseconds(config.max_age_ms)) {
                    Event(VT_EVENT_FRAME_DROPPED, VT_NOT_READY);
                    continue;
                }
                if (callbacks.on_encoded_frame)
                    callbacks.on_encoded_frame(callbacks.user, &item->encoded->info);
                if (has_media && media.decode)
                    media.decode(media.context, &item->encoded->info);
            } else if (callbacks.on_raw_frame) {
                if (Clock::now() - item->time > std::chrono::milliseconds(config.max_age_ms)) {
                    Event(VT_EVENT_FRAME_DROPPED, VT_NOT_READY);
                    continue;
                }
                callbacks.on_raw_frame(callbacks.user, &item->raw);
            }
        }
    }
};

extern "C" {
static void TransportFrame(void* user, const vt_encoded_frame* frame) {
    try { static_cast<vt_session*>(user)->Receive(frame); } catch (...) {}
}
static void TransportEvent(void* user, vt_event_type type, vt_status status) {
    try { static_cast<vt_session*>(user)->Event(type, status); } catch (...) {}
}
static void MediaEncoded(void* user, const vt_encoded_frame* frame) {
    vt_session_submit_encoded(static_cast<vt_session*>(user), frame);
}
static void MediaRaw(void* user, const vt_raw_frame* frame) {
    try { static_cast<vt_session*>(user)->ReceiveRaw(frame); } catch (...) {}
}
static vt_status ControlRequest(void* user, const vt_control_request* request) {
    auto* s = static_cast<vt_session*>(user);
    if (!request || request->struct_size < sizeof(vt_control_request)) return VT_INVALID_ARGUMENT;
    if (request->action == VT_CONTROL_REQUEST_KEYFRAME && s->has_media &&
        s->media.request_keyframe)
        return s->media.request_keyframe(s->media.context);
    return VT_UNSUPPORTED;
}

vt_status vt_session_create(const vt_session_config* config, const vt_callbacks* callbacks,
                            const vt_backends* backends, vt_session** out) {
    if (!out || !config || !callbacks || !backends || !backends->transport ||
        config->struct_size < sizeof(vt_session_config) || config->abi_version != VT_ABI_VERSION ||
        callbacks->struct_size < sizeof(vt_callbacks) || backends->struct_size < sizeof(vt_backends) ||
        backends->transport->struct_size < sizeof(vt_transport_backend) ||
        backends->transport->abi_version != VT_ABI_VERSION ||
        !backends->transport->start || !backends->transport->send ||
        !backends->transport->stop || !backends->transport->destroy ||
        (config->role != VT_SKY && config->role != VT_GROUND) ||
        (config->input_mode != VT_INPUT_ENCODED && config->input_mode != VT_INPUT_RAW) ||
        !config->max_frames || !config->max_bytes || !config->max_frame_bytes ||
        config->max_frame_bytes > config->max_bytes || !config->max_age_ms)
        return VT_INVALID_ARGUMENT;
    if (backends->media && (backends->media->struct_size < sizeof(vt_media_backend) ||
                            backends->media->abi_version != VT_ABI_VERSION ||
                            !backends->media->start || !backends->media->stop ||
                            !backends->media->destroy)) return VT_INVALID_ARGUMENT;
    if (backends->control && (backends->control->struct_size < sizeof(vt_control_backend) ||
                              backends->control->abi_version != VT_ABI_VERSION ||
                              !backends->control->start || !backends->control->stop ||
                              !backends->control->destroy)) return VT_INVALID_ARGUMENT;
    if (config->input_mode == VT_INPUT_RAW && (!backends->media || !backends->media->encode))
        return VT_UNSUPPORTED;
    vt_session* s = nullptr;
    try { s = new vt_session; } catch (...) { return VT_IO_ERROR; }
    if (!s) return VT_IO_ERROR;
    s->config = *config;
    s->callbacks = *callbacks;
    s->transport = *backends->transport;
    if (backends->media) { s->media = *backends->media; s->has_media = true; }
    if (backends->control) { s->control = *backends->control; s->has_control = true; }
    *out = s;
    return VT_OK;
}
vt_status vt_session_start(vt_session* s) {
    if (!s) return VT_INVALID_ARGUMENT;
    {
        std::lock_guard lock(s->mutex);
        if (s->used) return VT_NOT_READY;
        s->used = true;
    }
    vt_transport_sink ts{sizeof(ts), s, TransportFrame, TransportEvent};
    auto rc = s->transport.start(s->transport.context, s->config.role, &ts);
    if (rc != VT_OK) return rc;
    if (s->has_media) {
        vt_media_sink ms{sizeof(ms), s, MediaEncoded, MediaRaw};
        rc = s->media.start(s->media.context, s->config.role, &ms);
        if (rc != VT_OK) { s->transport.stop(s->transport.context); return rc; }
    }
    if (s->has_control) {
        vt_control_sink cs{sizeof(cs), s, ControlRequest};
        rc = s->control.start(s->control.context, s->config.role, &cs);
        if (rc != VT_OK) {
            if (s->has_media) s->media.stop(s->media.context);
            s->transport.stop(s->transport.context);
            return rc;
        }
    }
    try {
        {
            std::lock_guard lock(s->mutex);
            s->running = true;
            s->state = {sizeof(vt_event), VT_EVENT_READY, VT_OK};
            s->incoming.emplace_back(VT_EVENT_READY, VT_OK);
        }
        s->worker = std::thread([s] { s->SendLoop(); });
        s->dispatcher = std::thread([s] { s->DispatchLoop(); });
    } catch (...) {
        {
            std::lock_guard lock(s->mutex);
            s->running = false;
        }
        s->cv.notify_all();
        if (s->worker.joinable()) s->worker.join();
        if (s->has_control) s->control.stop(s->control.context);
        if (s->has_media) s->media.stop(s->media.context);
        s->transport.stop(s->transport.context);
        return VT_IO_ERROR;
    }
    s->cv.notify_all();
    return VT_OK;
}
vt_status vt_session_submit_encoded(vt_session* s, const vt_encoded_frame* f) {
    if (!s || !Valid(f, s->config.max_frame_bytes)) return VT_INVALID_ARGUMENT;
    if (s->config.role != VT_SKY) return VT_NOT_READY;
    try {
        auto owned = std::make_unique<Frame>(*f);
        std::lock_guard lock(s->mutex);
        if (!s->running) return VT_NOT_READY;
        bool dropped = false;
        while (!s->outgoing.empty() &&
               (s->outgoing.size() >= s->config.max_frames ||
                s->outgoing_bytes + owned->bytes.size() > s->config.max_bytes)) {
            s->outgoing_bytes -= s->outgoing.front().bytes;
            s->outgoing.pop_front();
            dropped = true;
        }
        s->outgoing_bytes += owned->bytes.size();
        s->outgoing.emplace_back(std::move(owned));
        if (dropped) s->EventLocked(VT_EVENT_FRAME_DROPPED, VT_QUEUE_FULL);
        s->cv.notify_all();
        return VT_OK;
    } catch (...) {
        return VT_IO_ERROR;
    }
}
vt_status vt_session_submit_raw(vt_session* s, const vt_raw_frame* f) {
    if (!s || !Valid(f)) return VT_INVALID_ARGUMENT;
    if (s->config.role != VT_SKY || s->config.input_mode != VT_INPUT_RAW) return VT_NOT_READY;
    const size_t pixels = size_t(f->width) * f->height;
    if (pixels > std::numeric_limits<size_t>::max() / 4) return VT_INVALID_ARGUMENT;
    const size_t bytes = pixels * 4;
    if (bytes > s->config.max_bytes) return VT_QUEUE_FULL;
    try {
        std::lock_guard lock(s->mutex);
        if (!s->running) return VT_NOT_READY;
        bool dropped = false;
        while (!s->outgoing.empty() &&
               (s->outgoing.size() >= s->config.max_frames ||
                s->outgoing_bytes + bytes > s->config.max_bytes)) {
            s->outgoing_bytes -= s->outgoing.front().bytes;
            s->outgoing.pop_front();
            dropped = true;
        }
        f->retain(f->native_buffer);
        try { s->outgoing.emplace_back(*f, bytes); }
        catch (...) { f->release(f->native_buffer); throw; }
        s->outgoing_bytes += bytes;
        if (dropped) s->EventLocked(VT_EVENT_FRAME_DROPPED, VT_QUEUE_FULL);
        s->cv.notify_all();
        return VT_OK;
    } catch (...) {
        return VT_IO_ERROR;
    }
}
vt_status vt_session_drain(vt_session* s, uint32_t timeout_ms) {
    if (!s || !timeout_ms) return VT_INVALID_ARGUMENT;
    std::unique_lock lock(s->mutex);
    if (!s->running) return VT_NOT_READY;
    return s->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                          [&] { return !s->running || (s->outgoing.empty() && !s->sending); })
        ? (s->running ? VT_OK : VT_CANCELLED) : VT_NOT_READY;
}
vt_status vt_session_get_state(const vt_session* s, vt_event* out) {
    if (!s || !out || out->struct_size < sizeof(vt_event)) return VT_INVALID_ARGUMENT;
    auto* mutable_s = const_cast<vt_session*>(s);
    std::lock_guard lock(mutable_s->mutex);
    *out = s->state;
    return VT_OK;
}
vt_status vt_session_send_control(vt_session* s, const vt_control_request* request) {
    if (!s || !request || request->struct_size < sizeof(vt_control_request)) return VT_INVALID_ARGUMENT;
    {
        std::lock_guard lock(s->mutex);
        if (!s->running) return VT_NOT_READY;
        if (!s->has_control || !s->control.send) return VT_UNSUPPORTED;
        ++s->control_inflight;
    }
    vt_status result = VT_IO_ERROR;
    try { result = s->control.send(s->control.context, request); } catch (...) {}
    {
        std::lock_guard lock(s->mutex);
        --s->control_inflight;
    }
    s->cv.notify_all();
    return result;
}
vt_status vt_session_stop(vt_session* s) {
    if (!s) return VT_INVALID_ARGUMENT;
    if (s->dispatcher.joinable() && s->dispatcher.get_id() == std::this_thread::get_id())
        return VT_NOT_READY;
    {
        std::lock_guard lock(s->mutex);
        if (!s->running) return VT_OK;
        s->running = false;
        s->state = {sizeof(vt_event), VT_EVENT_LINK_DOWN, VT_CANCELLED};
    }
    s->cv.notify_all();
    if (s->worker.joinable()) s->worker.join();
    {
        std::unique_lock lock(s->mutex);
        s->cv.wait(lock, [&] { return s->control_inflight == 0; });
    }
    if (s->has_control) s->control.stop(s->control.context);
    s->transport.stop(s->transport.context);
    if (s->dispatcher.joinable()) s->dispatcher.join();
    if (s->has_media) s->media.stop(s->media.context);
    {
        std::lock_guard lock(s->mutex);
        s->outgoing.clear();
        s->incoming.clear();
        s->outgoing_bytes = s->incoming_bytes = 0;
    }
    return VT_OK;
}
void vt_session_destroy(vt_session* s) {
    if (!s) return;
    if (vt_session_stop(s) != VT_OK) return;
    if (s->has_control) s->control.destroy(s->control.context);
    if (s->has_media) s->media.destroy(s->media.context);
    s->transport.destroy(s->transport.context);
    delete s;
}
}
