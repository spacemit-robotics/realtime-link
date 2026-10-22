#include <cstdio>
#include <video_transmission/latency.hpp>

#include "session_common.hpp"
#include "transport/frame_timing.hpp"
namespace video_transmission {
struct SkySession::Impl {
    struct Item {
        EncodedFrame frame;
        detail::Clock::time_point enqueued;
        detail::Clock::time_point deadline;
        detail::Clock::time_point nominal_finish;
    };
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Item> queue;
    std::thread worker;
    SessionConfig config;
    sockaddr_in destination{};
    int fd = -1;
    std::unique_ptr<detail::UsbLink> usb;
    size_t queued_bytes = 0;
    bool running = false;
    std::array<bool, 2> wait_keyframe{true, true};
    uint64_t sent_frames = 0, queue_expired = 0, transfer_expired = 0, send_errors = 0;
    detail::Clock::time_point head_wire_finish{};  // protected by mutex, zero when idle

    void Run() {
        std::array<uint8_t, legacy_udp::kPacketBytes> packet{};
        auto next_send = detail::Clock::now();
        for (;;) {
            Item item;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&] { return !running || !queue.empty(); });
                if (!running) break;
                item = std::move(queue.front());
                queued_bytes -= item.frame.size;
                queue.pop_front();
                if (wait_keyframe[item.frame.channel] && !item.frame.keyframe) continue;
                if (detail::Clock::now() > item.deadline) {
                    ++queue_expired;
                    latency::Mark(
                        "tx_queue_expired", item.frame.timestamp_us, item.frame.frame_number);
                    wait_keyframe.fill(true);
                    queue.clear();
                    queued_bytes = 0;
                    continue;
                }
                if (item.frame.keyframe) wait_keyframe[item.frame.channel] = false;
                if (!usb) {
                    // Keep the admission-time schedule: actual stalls must not refresh
                    // the jitter allowance of frames submitted behind this one.
                    head_wire_finish = item.nominal_finish;
                }
            }
            const auto& f = item.frame;
            if (usb) {
                if (!usb->Send(f)) {
                    std::lock_guard<std::mutex> lock(mutex);
                    wait_keyframe.fill(true);
                    queue.clear();
                    queued_bytes = 0;
                }
                continue;
            }
            const size_t count =
                (f.size + legacy_udp::kPayloadBytes - 1) / legacy_udp::kPayloadBytes;
            const auto deadline = detail::Clock::now() + detail::UdpFrameTimeout(config, f.size);
            legacy_udp::Header h;
            h.width = f.width;
            h.height = f.height;
            h.timestamp_us = f.timestamp_us;
            h.frame_number = f.frame_number;
            h.frame_len = static_cast<uint32_t>(f.size);
            h.frame_type = f.keyframe ? 0 : 1;
            h.channel = f.channel;
            h.flags =
                (f.keyframe ? 1u : 0u) | (f.parameter_sets ? 2u : 0u) | (f.channel == 1 ? 4u : 0u);
            h.total_packets = static_cast<uint16_t>(count);
            for (size_t i = 0; i < count; ++i) {
                h.packet_seq = static_cast<uint16_t>(i);
                const auto header = legacy_udp::Encode(h);
                std::copy(header.begin(), header.end(), packet.begin());
                const size_t offset = i * legacy_udp::kPayloadBytes;
                const size_t length = std::min(legacy_udp::kPayloadBytes, f.size - offset);
                std::memcpy(packet.data() + header.size(), f.data.get() + offset, length);
                const auto now = detail::Clock::now();
                if (next_send < now) next_send = now;
                while (detail::Clock::now() < next_send) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (!running) return;
                    }
                    const auto remaining = next_send - detail::Clock::now();
                    if (remaining > detail::Clock::duration::zero())
                        std::this_thread::sleep_for(std::min(remaining,
                            std::chrono::duration_cast<detail::Clock::duration>(
                                std::chrono::milliseconds(5))));
                }
                if (detail::Clock::now() > deadline) {
                    ++transfer_expired;
                    latency::Mark("tx_transfer_expired", f.timestamp_us, f.frame_number);
                    std::lock_guard<std::mutex> lock(mutex);
                    wait_keyframe.fill(true);
                    queue.clear();
                    queued_bytes = 0;
                    break;
                }
                const auto packet_time = std::chrono::duration<double>(
                    (header.size() + length) * 8.0 / (config.rate_mbps * 1000000.0));
                next_send += std::chrono::duration_cast<detail::Clock::duration>(packet_time);
                if (i == 0) latency::Mark("tx_first", f.timestamp_us, f.frame_number);
                if (sendto(fd, packet.data(), header.size() + length, 0,
                        reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) < 0) {
                    ++send_errors;
                    std::lock_guard<std::mutex> lock(mutex);
                    wait_keyframe.fill(true);
                    queue.clear();
                    queued_bytes = 0;
                    break;
                }
                if (i + 1 == count) {
                    ++sent_frames;
                    latency::Mark("tx_last", f.timestamp_us, f.frame_number);
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                head_wire_finish = {};
            }
        }
    }
};

SkySession::SkySession() : impl_(new Impl) {}
SkySession::~SkySession() { StopSending(); }
bool SkySession::Start(const SessionConfig& c) {
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.running || c.channel > 1 || c.packet_size_bytes != legacy_udp::kPacketBytes ||
        !detail::ValidLimits(c.limits) ||
        !std::isfinite(c.rate_mbps) || !(c.rate_mbps > 0) ||
        (c.transport == Transport::Udp && !detail::ValidUdpTiming(c)) ||
        (c.transport == Transport::Udp && !detail::ParseAddress(c.address, c.port, &s.destination)))
        return false;
    if (c.transport == Transport::Usb) {
        s.usb = std::make_unique<detail::UsbLink>();
        if (!s.usb->Open(c, true)) {
            s.usb.reset();
            return false;
        }
    } else {
        s.fd = detail::OpenSocket(c, SOCK_DGRAM, false);
        if (s.fd < 0) return false;
    }
    s.config = c;
    s.sent_frames = s.queue_expired = s.transfer_expired = s.send_errors = 0;
    s.head_wire_finish = {};
    s.wait_keyframe.fill(true);
    s.running = true;
    s.worker = std::thread([&s] { s.Run(); });
    return true;
}
SubmitResult SkySession::SubmitEncodedFrame(EncodedFrame f) {
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.running) return SubmitResult::NotReady;
    if (!detail::ValidFrame(f, s.config.limits)) return SubmitResult::InvalidFrame;
    if (f.channel != s.config.channel) return SubmitResult::InvalidFrame;
    if (s.wait_keyframe[f.channel] && !f.keyframe) return SubmitResult::NotReady;
    if (s.queue.size() >= s.config.limits.max_frames ||
        f.size > s.config.limits.max_bytes - s.queued_bytes) {
        s.queue.clear();
        s.queued_bytes = 0;
        s.wait_keyframe.fill(true);
        return SubmitResult::QueueFull;
    }
    if (f.keyframe) s.wait_keyframe[f.channel] = false;
    s.queued_bytes += f.size;
    latency::Mark("tx_enqueue", f.timestamp_us, f.frame_number);
    const auto admitted = detail::Clock::now();
    const auto previous_finish =
        s.queue.empty() ? s.head_wire_finish : s.queue.back().nominal_finish;
    const auto deadline = s.usb ? admitted + s.config.limits.max_age
                                : detail::UdpQueueDeadline(s.config, admitted, previous_finish);
    const auto nominal_finish = s.usb ? admitted
                                        : deadline - s.config.limits.max_age +
            std::chrono::duration_cast<detail::Clock::duration>(
                std::chrono::duration<double>(detail::UdpWireSeconds(f.size, s.config.rate_mbps)));
    latency::Mark("tx_queue_deadline", f.timestamp_us,
        std::chrono::duration_cast<std::chrono::microseconds>(deadline.time_since_epoch()).count());
    s.queue.push_back({std::move(f), admitted, deadline, nominal_finish});
    s.cv.notify_one();
    return SubmitResult::Accepted;
}
bool SkySession::Ready() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->running && (!impl_->usb || impl_->usb->Active());
}
void SkySession::StopSending() {
    auto& s = *impl_;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.running) return;
        s.running = false;
        s.queue.clear();
        s.queued_bytes = 0;
    }
    s.cv.notify_all();
    if (s.usb) s.usb->Cancel();
    if (s.worker.joinable()) s.worker.join();
    if (!s.usb) {
        std::fprintf(stderr,
            "[VIDEO_TRANSMISSION_TX] complete=%llu queue_expired=%llu transfer_expired=%llu "
            "send_errors=%llu\n",
            static_cast<unsigned long long>(s.sent_frames),
            static_cast<unsigned long long>(s.queue_expired),
            static_cast<unsigned long long>(s.transfer_expired),
            static_cast<unsigned long long>(s.send_errors));
    }
    s.usb.reset();
    if (s.fd >= 0) close(s.fd);
    s.fd = -1;
}

}  // namespace video_transmission
