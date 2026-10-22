#include <video_transmission/latency.hpp>

#include "session_common.hpp"
#include "transport/frame_timing.hpp"
namespace video_transmission {
struct GroundSession::Impl {
    using Key = std::tuple<uint32_t, uint64_t, uint32_t>;
    using AuKey = std::pair<uint32_t, uint64_t>;
    struct Assembly {
        legacy_udp::Header header;
        std::vector<uint8_t> bytes;
        std::vector<bool> received;
        size_t count = 0;
        detail::Clock::time_point created;
    };
    struct AuAssembly {
        std::vector<std::vector<uint8_t>> slices;
        legacy_udp::Header header;
        size_t count = 0;
        size_t bytes = 0;
        bool keyframe = false;
        bool parameter_sets = false;
        bool has_end = false;
        detail::Clock::time_point created;
    };
    std::mutex mutex;
    std::thread worker, dispatcher;
    std::condition_variable dispatch_cv;
    struct Output {
        EncodedFrame frame;
        detail::Clock::time_point at;
    };
    std::deque<Output> outputs;
    std::deque<Event> events;
    size_t output_bytes = 0;
    bool output_keyframe = true;
    SessionConfig config;
    int fd = -1;
    std::unique_ptr<detail::UsbLink> usb;
    bool running = false;
    detail::Clock::time_point last_arrival[2]{};
    bool have_last[2] = {false, false};
    uint64_t last[2] = {0, 0};
    bool wait_keyframe[2] = {true, true};
    std::map<Key, Assembly> assemblies;
    std::map<AuKey, AuAssembly> access_units;
    size_t assembly_bytes = 0;
    std::function<void(EncodedFrame)> on_frame;
    std::function<void(Event)> on_event;

    void Emit(Event event) {
        std::lock_guard lock(mutex);
        if (events.size() >= 16) events.pop_front();
        events.push_back(event);
        dispatch_cv.notify_all();
    }
    void Dispatch() {
        for (;;) {
            EncodedFrame frame;
            std::deque<Event> pending;
            std::function<void(EncodedFrame)> callback;
            std::function<void(Event)> event_callback;
            {
                std::unique_lock lock(mutex);
                dispatch_cv.wait(
                    lock, [&] { return !running || !outputs.empty() || !events.empty(); });
                if (!running) break;
                pending.swap(events);
                callback = on_frame;
                event_callback = on_event;
                if (!outputs.empty()) {
                    auto item = std::move(outputs.front());
                    outputs.pop_front();
                    output_bytes -= item.frame.size;
                    if (detail::Clock::now() - item.at > config.limits.max_age) {
                        outputs.clear();
                        output_bytes = 0;
                        output_keyframe = true;
                        pending.push_back(Event::WaitingForKeyframe);
                    } else
                        frame = std::move(item.frame);
                }
            }
            try {
                for (auto e : pending)
                    if (event_callback) event_callback(e);
                if (frame.data && callback) callback(std::move(frame));
            } catch (...) {
            }
        }
    }
    void Drop(std::map<Key, Assembly>::iterator it) {
        wait_keyframe[std::get<0>(it->first)] = true;
        assembly_bytes -= it->second.bytes.size();
        assemblies.erase(it);
        Emit(Event::WaitingForKeyframe);
    }
    void DropAu(std::map<AuKey, AuAssembly>::iterator it) {
        wait_keyframe[it->first.first] = true;
        assembly_bytes -= it->second.bytes;
        access_units.erase(it);
        Emit(Event::WaitingForKeyframe);
    }
    void Deliver(EncodedFrame f) {
        if (f.channel != config.channel) return;
        latency::Mark("rx_complete", f.timestamp_us, f.frame_number);
        if (have_last[f.channel] && f.frame_number <= last[f.channel]) return;
        if (have_last[f.channel] && f.frame_number != last[f.channel] + 1 && !f.keyframe) {
            wait_keyframe[f.channel] = true;
            Emit(Event::WaitingForKeyframe);
        }

        if (wait_keyframe[f.channel]) {
            if (!f.keyframe) return;
            wait_keyframe[f.channel] = false;
            Emit(Event::StreamResumed);
        }
        have_last[f.channel] = true;
        last[f.channel] = f.frame_number;
        last_arrival[f.channel] = detail::Clock::now();
        std::lock_guard lock(mutex);
        if (!running) return;
        if (outputs.size() >= config.limits.max_frames ||
            f.size > config.limits.max_bytes - output_bytes) {
            outputs.clear();
            output_bytes = 0;
            output_keyframe = true;
            if (events.size() >= 16) events.pop_front();
            events.push_back(Event::WaitingForKeyframe);
        }
        if (output_keyframe && !f.keyframe) {
            dispatch_cv.notify_all();
            return;
        }
        output_keyframe = false;
        output_bytes += f.size;
        outputs.push_back({std::move(f), detail::Clock::now()});
        dispatch_cv.notify_all();
    }

    void Expire(detail::Clock::time_point now) {
        for (unsigned channel = 0; channel < 2; ++channel)
            if (have_last[channel] && now - last_arrival[channel] > config.limits.max_age) {
                have_last[channel] = false;
                wait_keyframe[channel] = true;
                Emit(Event::StreamInterrupted);
                Emit(Event::WaitingForKeyframe);
            }
        for (auto it = assemblies.begin(); it != assemblies.end();) {
            if (now - it->second.created >
                detail::UdpFrameTimeout(config, it->second.header.frame_len)) {
                auto old = it++;
                Drop(old);
            } else
                ++it;
        }
        for (auto it = access_units.begin(); it != access_units.end();) {
            // A sliced AU does not advertise its total size. Keep a fixed deadline
            // bounded by the configured maximum AU size, not a per-packet idle timer.
            if (now - it->second.created >
                detail::UdpFrameTimeout(config, config.limits.max_frame_bytes)) {
                auto old = it++;
                DropAu(old);
            } else
                ++it;
        }
    }
    void Run() {
        std::array<uint8_t, legacy_udp::kPacketBytes> packet{};
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!running) break;
            }
            if (usb) {
                EncodedFrame f;
                if (usb->Receive(&f)) Deliver(std::move(f));
                if (!usb->Active()) {
                    Emit(Event::StreamInterrupted);
                    break;
                }
                continue;
            }
            Expire(detail::Clock::now());
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            timeval timeout{0, 100000};
            if (select(fd + 1, &fds, nullptr, nullptr, &timeout) <= 0) continue;
            sockaddr_in source{};
            socklen_t source_len = sizeof(source);
            const ssize_t n = recvfrom(fd, packet.data(), packet.size(), 0,
                reinterpret_cast<sockaddr*>(&source), &source_len);
            const auto arrival_us = latency::Enabled() ? latency::NowUs() : 0;
            if (n <= 0) continue;
            if (!config.peer.empty()) {
                in_addr peer{};
                if (inet_pton(AF_INET, config.peer.c_str(), &peer) != 1 ||
                    source.sin_addr.s_addr != peer.s_addr)
                    continue;
            }
            legacy_udp::Header h;
            if (!legacy_udp::Decode(packet.data(), static_cast<size_t>(n), &h) || h.channel > 1 ||
                h.frame_len > config.limits.max_frame_bytes || h.width > 16384 ||
                h.height > 16384 || h.slice_count > 256 || h.slice_index >= h.slice_count) {
                Emit(Event::InvalidPacket);
                continue;
            }
            if (h.channel != config.channel) continue;
            const size_t payload = static_cast<size_t>(n) - legacy_udp::kHeaderBytes;
            const size_t expected_count =
                (h.frame_len + legacy_udp::kPayloadBytes - 1) / legacy_udp::kPayloadBytes;
            const size_t offset = size_t(h.packet_seq) * legacy_udp::kPayloadBytes;
            if (payload == 0 || h.total_packets != expected_count ||
                offset + payload > h.frame_len ||
                (h.packet_seq + 1 < h.total_packets && payload != legacy_udp::kPayloadBytes) ||
                (h.packet_seq + 1 == h.total_packets && offset + payload != h.frame_len)) {
                Emit(Event::InvalidPacket);
                continue;
            }
            const auto now = detail::Clock::now();
            const Key key{h.channel, h.frame_number, h.slice_index};
            auto it = assemblies.find(key);
            if (it == assemblies.end()) {
                while ((!assemblies.empty() || !access_units.empty()) &&
                    (assemblies.size() + access_units.size() >= config.limits.max_frames ||
                        h.frame_len > config.limits.max_bytes - assembly_bytes)) {
                    if (!assemblies.empty())
                        Drop(assemblies.begin());
                    else
                        DropAu(access_units.begin());
                }
                if (h.frame_len > config.limits.max_bytes - assembly_bytes) continue;
                latency::Mark("rx_first", h.timestamp_us, h.frame_number, arrival_us);
                Assembly a;
                a.header = h;
                a.bytes.resize(h.frame_len);
                a.received.resize(h.total_packets);
                a.created = now;
                assembly_bytes += h.frame_len;
                it = assemblies.emplace(key, std::move(a)).first;
            }
            auto& a = it->second;
            if (a.header.frame_len != h.frame_len || a.header.total_packets != h.total_packets ||
                a.header.width != h.width || a.header.height != h.height ||
                a.header.timestamp_us != h.timestamp_us || a.header.slice_count != h.slice_count ||
                a.header.flags != h.flags || a.header.frame_type != h.frame_type ||
                a.header.end_of_frame != h.end_of_frame) {
                Drop(it);
                continue;
            }
            if (a.received[h.packet_seq]) continue;
            // Receiving a large frame is progress, not a stream interruption merely
            // because no COMPLETE frame has been delivered in the last max_age.
            last_arrival[h.channel] = now;
            std::memcpy(a.bytes.data() + offset, packet.data() + legacy_udp::kHeaderBytes, payload);
            a.received[h.packet_seq] = true;
            if (++a.count != h.total_packets) continue;
            EncodedFrame f;
            f.size = a.bytes.size();
            f.data = detail::Own(std::move(a.bytes));
            f.width = h.width;
            f.height = h.height;
            f.channel = h.channel;
            f.frame_number = h.frame_number;
            f.timestamp_us = h.timestamp_us;
            f.keyframe = h.frame_type == 0 || (h.flags & 1);
            f.parameter_sets = (h.flags & 2) != 0;
            assembly_bytes -= f.size;
            assemblies.erase(it);
            if (h.slice_count == 1) {
                Deliver(std::move(f));
                continue;
            }
            const AuKey au_key{h.channel, h.frame_number};
            auto au_it = access_units.find(au_key);
            if (au_it == access_units.end()) {
                AuAssembly au;
                au.slices.resize(h.slice_count);
                au.header = h;
                au.created = now;
                au_it = access_units.emplace(au_key, std::move(au)).first;
            }
            auto& au = au_it->second;
            if (au.header.slice_count != h.slice_count || au.header.width != h.width ||
                au.header.height != h.height || au.header.timestamp_us != h.timestamp_us) {
                DropAu(au_it);
                continue;
            }
            if (!au.slices[h.slice_index].empty()) continue;
            while ((!assemblies.empty() || access_units.size() > 1) &&
                f.size > config.limits.max_bytes - assembly_bytes) {
                if (!assemblies.empty())
                    Drop(assemblies.begin());
                else {
                    auto old = access_units.begin();
                    if (old == au_it) ++old;
                    DropAu(old);
                }
            }
            if (f.size > config.limits.max_bytes - assembly_bytes) {
                DropAu(au_it);
                continue;
            }
            if (f.size >
                config.limits.max_frame_bytes - std::min(au.bytes, config.limits.max_frame_bytes)) {
                DropAu(au_it);
                continue;
            }
            au.bytes += f.size;
            au.keyframe = au.keyframe || f.keyframe;
            au.parameter_sets = au.parameter_sets || f.parameter_sets;
            if (h.slice_index + 1 == h.slice_count) au.has_end = h.end_of_frame != 0;
            assembly_bytes += f.size;
            au.slices[h.slice_index].assign(f.data.get(), f.data.get() + f.size);
            if (++au.count != au.slices.size()) continue;
            if (!au.has_end) {
                DropAu(au_it);
                continue;
            }
            std::vector<uint8_t> joined;
            joined.reserve(au.bytes);
            for (const auto& slice : au.slices)
                joined.insert(joined.end(), slice.begin(), slice.end());
            f.size = joined.size();
            f.data = detail::Own(std::move(joined));
            f.keyframe = au.keyframe;
            f.parameter_sets = au.parameter_sets;
            assembly_bytes -= au.bytes;
            access_units.erase(au_it);
            Deliver(std::move(f));
        }
        assemblies.clear();
        access_units.clear();
        assembly_bytes = 0;
    }
};

GroundSession::GroundSession() : impl_(new Impl) {}
GroundSession::~GroundSession() { StopReceiving(); }
bool GroundSession::Start(const SessionConfig& c) {
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    sockaddr_in address{};
    if (s.running || c.channel > 1 || c.packet_size_bytes != legacy_udp::kPacketBytes ||
        !detail::ValidLimits(c.limits) ||
        (c.transport == Transport::Udp && !detail::ValidUdpTiming(c)) ||
        (c.transport == Transport::Udp && !detail::ParseAddress(c.address, c.port, &address)))
        return false;
    if (!c.peer.empty()) {
        in_addr peer{};
        if (inet_pton(AF_INET, c.peer.c_str(), &peer) != 1) return false;
    }
    if (c.transport == Transport::Usb) {
        s.usb = std::make_unique<detail::UsbLink>();
        if (!s.usb->Open(c, false)) {
            s.usb.reset();
            return false;
        }
    } else {
        s.fd = detail::OpenSocket(c, SOCK_DGRAM, true);
        if (s.fd < 0) return false;
    }
    s.config = c;
    s.wait_keyframe[0] = s.wait_keyframe[1] = true;
    s.have_last[0] = s.have_last[1] = false;
    s.running = true;
    s.output_keyframe = true;
    s.output_bytes = 0;
    s.outputs.clear();
    s.events.clear();
    s.dispatcher = std::thread([&s] { s.Dispatch(); });
    s.worker = std::thread([&s] { s.Run(); });
    return true;
}
void GroundSession::OnEncodedFrame(std::function<void(EncodedFrame)> callback) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->on_frame = std::move(callback);
}
void GroundSession::OnEvent(std::function<void(Event)> callback) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->on_event = std::move(callback);
}
void GroundSession::StopReceiving() {
    auto& s = *impl_;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.running) return;
        s.running = false;
    }
    s.dispatch_cv.notify_all();
    if (s.usb) s.usb->Cancel();
    if (s.worker.joinable()) s.worker.join();
    if (s.dispatcher.joinable()) s.dispatcher.join();
    s.outputs.clear();
    s.events.clear();
    s.output_bytes = 0;
    s.usb.reset();
    if (s.fd >= 0) close(s.fd);
    s.fd = -1;
}

}  // namespace video_transmission
