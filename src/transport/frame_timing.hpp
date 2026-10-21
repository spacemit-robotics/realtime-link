#ifndef FRAME_TIMING_HPP
#define FRAME_TIMING_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <video_transmission/session.hpp>

#include "protocol/legacy_udp.hpp"

namespace video_transmission::detail {

using FrameClock = std::chrono::steady_clock;

inline double UdpWireSeconds(size_t bytes, double rate_mbps) {
    const size_t packets =
        bytes / legacy_udp::kPayloadBytes + (bytes % legacy_udp::kPayloadBytes != 0);
    return (static_cast<double>(bytes) + static_cast<double>(packets) * legacy_udp::kHeaderBytes) *
        8.0 / (rate_mbps * 1000000.0);
}

inline bool ValidUdpTiming(const SessionConfig& config) {
    if (!std::isfinite(config.rate_mbps) || config.rate_mbps <= 0) return false;
    // Include the bounded FIFO and one in-flight frame. Packet rounding costs
    // at most one additional header per queued frame.
    const double seconds = UdpWireSeconds(config.limits.max_bytes, config.rate_mbps) +
        UdpWireSeconds(config.limits.max_frame_bytes, config.rate_mbps) +
        static_cast<double>(config.limits.max_frames) * legacy_udp::kHeaderBytes * 8.0 /
            (config.rate_mbps * 1000000.0) +
        std::chrono::duration<double>(config.limits.max_age).count();
    // Reject unrepresentable deadlines before converting a user-provided rate to clock ticks.
    return std::isfinite(seconds) && seconds >= 0 &&
        seconds < std::chrono::duration<double>(FrameClock::duration::max()).count() / 4;
}

// In flight, allow the declared frame's serialization time PLUS max_age for
// scheduling/network jitter. The deadline never
// slides with packet arrival; max_frame_bytes and rate_mbps bound the largest budget.
inline FrameClock::duration UdpFrameTimeout(const SessionConfig& config, size_t bytes) {
    return std::chrono::duration_cast<FrameClock::duration>(
        std::chrono::duration<double>(UdpWireSeconds(bytes, config.rate_mbps)) +
        config.limits.max_age);
}

// Credit nominal serialization of all predecessors, not their actual delays.
// The caller stores each frame's nominal finish at admission. Its deadline is
// fixed thereafter; later traffic cannot extend it. Count/byte limits still apply.
inline FrameClock::time_point UdpQueueDeadline(const SessionConfig& config,
    FrameClock::time_point admitted, FrameClock::time_point previous_wire_finish) {
    return std::max(admitted, previous_wire_finish) + config.limits.max_age;
}

// Used under the pipeline codec mutex. Repeated loss notifications for the same
// in-flight recovery frame must not force an IDR for every subsequent input frame.
class RecoveryGate {
public:
    bool Allow(FrameClock::time_point now, FrameClock::duration interval) {
        if (now < next_request_) return false;
        next_request_ = now + interval;
        return true;
    }

    void Reset() { next_request_ = {}; }

private:
    FrameClock::time_point next_request_{};
};

}  // namespace video_transmission::detail

#endif  // FRAME_TIMING_HPP
