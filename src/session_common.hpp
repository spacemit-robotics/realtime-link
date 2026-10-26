#ifndef SESSION_COMMON_HPP
#define SESSION_COMMON_HPP

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>
#include <video_transmission/session.hpp>

#include "protocol/legacy_udp.hpp"
#include "transport/socket.hpp"
#include "transport/usb.hpp"

namespace video_transmission {
namespace detail {
using Clock = std::chrono::steady_clock;
inline bool ValidLimits(const Limits& l) {
    return l.max_frames > 0 && l.max_bytes > 0 && l.max_frame_bytes > 0 &&
        l.max_frame_bytes <= l.max_bytes && l.max_age.count() > 0;
}
inline bool ParseAddress(const std::string& ip, uint16_t port, sockaddr_in* out) {
    if (!out || port == 0) return false;
    *out = {};
    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    return inet_pton(AF_INET, ip.c_str(), &out->sin_addr) == 1;
}
[[maybe_unused]] inline bool ValidFrame(const EncodedFrame& f, const Limits& l) {
    return f.data && f.size && f.size <= l.max_frame_bytes &&
        f.size <= std::numeric_limits<uint32_t>::max() && f.width && f.height && f.width <= 16384 &&
        f.height <= 16384 && f.channel <= 1 && f.codec == Codec::H265 &&
        (f.size + legacy_udp::kPayloadBytes - 1) / legacy_udp::kPayloadBytes <=
        std::numeric_limits<uint16_t>::max();
}
[[maybe_unused]] inline std::shared_ptr<const uint8_t> Own(std::vector<uint8_t>&& bytes) {
    auto owner = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
    return {owner, owner->data()};
}
}  // namespace detail

}  // namespace video_transmission

#endif  // SESSION_COMMON_HPP
