#ifndef COMMON_HPP
#define COMMON_HPP
#include <video_transmission/video_transmission.h>
#include <sys/vb_type.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <dlfcn.h>
#include <stdexcept>
#include <string>
#include <thread>

inline uint32_t native_number(const char* value) {
    if (!value || !*value || *value == 45 || *value == 43) throw std::invalid_argument("expected positive integer");
    char* end = nullptr;
    errno = 0;
    auto n = std::strtoul(value, &end, 10);
    if (errno || !end || *end || !n || n > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("expected positive integer");
    return static_cast<uint32_t>(n);
}
inline uint32_t native_offset(const char* value) {
    if (!value || !*value || *value == 45 || *value == 43) throw std::invalid_argument("expected nonnegative integer");
    char* end = nullptr;
    errno = 0;
    auto n = std::strtoul(value, &end, 10);
    if (errno || !end || *end || n > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("expected nonnegative integer");
    return static_cast<uint32_t>(n);
}
struct NativeRequestRetry {
    using Clock = std::chrono::steady_clock;
    bool pending = false;
    Clock::time_point next{};
    void signal() { pending = true; }
    bool ready(Clock::time_point now) const { return pending && now >= next; }
    void finish(vt_status status, Clock::time_point now) {
        if (status == VT_OK || status == VT_UNSUPPORTED)
            pending = false;
        else
            next = now + std::chrono::seconds(1);
    }
};
struct NativeEvents {
    std::atomic<bool> reconnect{false}, keyframe{false};
    std::atomic<unsigned long> frames{0};
};
inline void native_event(void* p, const vt_event* event) {
    auto& e = *static_cast<NativeEvents*>(p);
    if (event->type == VT_EVENT_LINK_DOWN) e.reconnect = true;
    if (event->type == VT_EVENT_KEYFRAME_NEEDED || event->type == VT_EVENT_FRAME_DROPPED) e.keyframe = true;
}
class NativePlugin {
    void* handle_ = nullptr;
    vt_backend_plugin_create_v1_fn factory_ = nullptr;

public:
    explicit NativePlugin(const char* path) {
        handle_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!handle_) throw std::runtime_error(dlerror());
        factory_ = reinterpret_cast<vt_backend_plugin_create_v1_fn>(dlsym(handle_, "vt_backend_plugin_create_v1"));
        if (!factory_) {
            dlclose(handle_);
            handle_ = nullptr;
            throw std::runtime_error("backend plugin entry missing");
        }
    }
    ~NativePlugin() {
        if (handle_) dlclose(handle_);
    }
    NativePlugin(const NativePlugin&) = delete;
    vt_status create(const char* cfg, vt_role role, vt_transport_backend* t, vt_control_backend* c) {
        return factory_(cfg, role, t, c);
    }
};
class NativeSession {
    vt_session* session_ = nullptr;
    vt_media_backend media_{};

public:
    ~NativeSession() { reset(); }
    void reset() {
        if (session_) {
            vt_session_destroy(session_);
            session_ = nullptr;
            media_ = {};
        }
    }
    vt_session* get() { return session_; }
    vt_status request_local_keyframe() {
        return session_ && media_.request_keyframe ? media_.request_keyframe(media_.context) : VT_NOT_READY;
    }
    vt_status open(NativePlugin& plugin, const char* cfg, vt_role role, NativeEvents& events, uint32_t width,
        uint32_t height, uint32_t fps, uint32_t bitrate, uint32_t gop, void (*on_raw)(void*, const vt_raw_frame*)) {
        reset();
        vt_transport_backend transport{};
        vt_control_backend control{};
        auto rc = plugin.create(cfg, role, &transport, &control);
        if (rc != VT_OK) return rc;
        vt_mpp_media_config mc{
            sizeof(mc), VT_ABI_VERSION, role, 0, width, height, fps, bitrate, gop, MPP_PIXEL_FORMAT_NV12};
        rc = vt_mpp_media_create(&mc, &media_);
        if (rc != VT_OK) {
            if (control.context) control.destroy(control.context);
            transport.destroy(transport.context);
            return rc;
        }
        vt_backends backends{sizeof(backends), &transport, &media_, control.context ? &control : nullptr};
        vt_callbacks callbacks{sizeof(callbacks), &events, native_event, nullptr, on_raw};
        vt_session_config sc{sizeof(sc), VT_ABI_VERSION, role, role == VT_SKY ? VT_INPUT_RAW : VT_INPUT_ENCODED, 8,
            32 * 1024 * 1024, 8 * 1024 * 1024, 1000};
        rc = vt_session_create(&sc, &callbacks, &backends, &session_);
        if (rc != VT_OK) {
            media_.destroy(media_.context);
            if (control.context) control.destroy(control.context);
            transport.destroy(transport.context);
            media_ = {};
            return rc;
        }
        events.reconnect = false;
        events.keyframe = false;
        rc = vt_session_start(session_);
        if (rc != VT_OK) {
            reset();
            return rc;
        }
        if (role == VT_SKY && media_.request_keyframe) media_.request_keyframe(media_.context);
        return VT_OK;
    }
};
inline vt_status request_keyframe(NativeSession& session) {
    vt_control_request request{sizeof(request), VT_CONTROL_REQUEST_KEYFRAME, 0};
    return session.get() ? vt_session_send_control(session.get(), &request) : VT_NOT_READY;
}
#endif
