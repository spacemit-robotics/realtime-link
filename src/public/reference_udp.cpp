#include <video_transmission/video_transmission.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr size_t kHeader = 36;
constexpr size_t kPayload = 1120;
constexpr size_t kDatagram = kHeader + kPayload;
using Clock = std::chrono::steady_clock;

void Put16(uint8_t* p, uint16_t x) { p[0] = x >> 8; p[1] = x; }
void Put32(uint8_t* p, uint32_t x) {
    for (int i = 3; i >= 0; --i) { p[i] = x; x >>= 8; }
}
void Put64(uint8_t* p, uint64_t x) {
    for (int i = 7; i >= 0; --i) { p[i] = x; x >>= 8; }
}
uint16_t Get16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
uint32_t Get32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint64_t Get64(const uint8_t* p) {
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i) x = (x << 8) | p[i];
    return x;
}
bool Address(const std::string& address, uint16_t port, sockaddr_in* out) {
    *out = {};
    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    return inet_pton(AF_INET, address.c_str(), &out->sin_addr) == 1;
}
struct Assembly {
    vt_encoded_frame frame{};
    std::vector<uint8_t> bytes;
    std::vector<bool> received;
    size_t count = 0;
    Clock::time_point created = Clock::now();
};
struct ReferenceUdp {
    std::string bind_address, peer_address;
    uint16_t port;
    size_t max_frame_bytes;
    vt_role role = VT_SKY;
    vt_transport_sink sink{};
    std::atomic<bool> running{false};
    int fd = -1;
    sockaddr_in peer{};
    std::thread receiver;
    std::map<std::pair<uint64_t, uint64_t>, Assembly> assemblies;

    void Run() {
        std::array<uint8_t, kDatagram> packet{};
        while (running) {
            sockaddr_in source{};
            socklen_t source_size = sizeof(source);
            const auto n = recvfrom(fd, packet.data(), packet.size(), 0,
                                    reinterpret_cast<sockaddr*>(&source), &source_size);
            if (!running) break;
            if (n <= 0) continue;
            if (!peer_address.empty() && source.sin_addr.s_addr != peer.sin_addr.s_addr) continue;
            const auto* p = packet.data();
            if (size_t(n) <= kHeader || std::memcmp(p, "VTR1", 4) || p[4] != 1) continue;
            const uint16_t count = Get16(p + 6), index = Get16(p + 8);
            const uint32_t total = Get32(p + 12);
            const uint32_t width = Get16(p + 16), height = Get16(p + 18);
            const uint64_t number = Get64(p + 20), pts = Get64(p + 28);
            const size_t payload = size_t(n) - kHeader;
            const size_t offset = size_t(index) * kPayload;
            if (!count || index >= count || !total || total > max_frame_bytes ||
                count != (total + kPayload - 1) / kPayload || !width || !height ||
                offset + payload > total ||
                (index + 1 < count && payload != kPayload) ||
                (index + 1 == count && offset + payload != total)) continue;
            const auto now = Clock::now();
            for (auto it = assemblies.begin(); it != assemblies.end();) {
                if (now - it->second.created > std::chrono::milliseconds(500))
                    it = assemblies.erase(it);
                else ++it;
            }
            const auto key = std::make_pair(number, pts);
            auto [it, created] = assemblies.try_emplace(key);
            auto& a = it->second;
            if (created) {
                if (assemblies.size() > 8) {
                    assemblies.erase(it);
                    continue;
                }
                a.bytes.resize(total);
                a.received.resize(count);
                a.frame = {sizeof(vt_encoded_frame), nullptr, total, width, height, number, pts,
                           uint32_t(p[5])};
            }
            if (a.bytes.size() != total || a.received.size() != count ||
                a.frame.width != width || a.frame.height != height || a.frame.flags != p[5]) {
                assemblies.erase(it);
                continue;
            }
            if (!a.received[index]) {
                std::memcpy(a.bytes.data() + offset, p + kHeader, payload);
                a.received[index] = true;
                ++a.count;
            }
            if (a.count == count) {
                a.frame.data = a.bytes.data();
                sink.on_frame(sink.user, &a.frame);
                assemblies.erase(it);
            }
        }
    }
};

vt_status Start(void* context, vt_role role, const vt_transport_sink* sink) {
    auto* s = static_cast<ReferenceUdp*>(context);
    if (!sink || sink->struct_size < sizeof(vt_transport_sink) || !sink->on_frame || !sink->on_event ||
        s->running || (role != VT_SKY && role != VT_GROUND)) return VT_INVALID_ARGUMENT;
    sockaddr_in local{};
    if (!Address(s->bind_address, role == VT_GROUND ? s->port : 0, &local) ||
        (!s->peer_address.empty() && !Address(s->peer_address, s->port, &s->peer)))
        return VT_INVALID_ARGUMENT;
    s->fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s->fd < 0) return VT_IO_ERROR;
    timeval timeout{0, 100000};
    setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int receive_buffer = 4 * 1024 * 1024;
    setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
    if (bind(s->fd, reinterpret_cast<sockaddr*>(&local), sizeof(local))) {
        close(s->fd);
        s->fd = -1;
        return VT_IO_ERROR;
    }
    s->role = role;
    s->sink = *sink;
    s->running = true;
    try {
        if (role == VT_GROUND) s->receiver = std::thread([s] {
            try { s->Run(); }
            catch (...) { s->sink.on_event(s->sink.user, VT_EVENT_ERROR, VT_IO_ERROR); }
        });
    } catch (...) {
        s->running = false;
        close(s->fd);
        s->fd = -1;
        return VT_IO_ERROR;
    }
    s->sink.on_event(s->sink.user, VT_EVENT_LINK_UP, VT_OK);
    return VT_OK;
}
vt_status Send(void* context, const vt_encoded_frame* frame) {
    auto* s = static_cast<ReferenceUdp*>(context);
    if (!s->running || s->role != VT_SKY) return VT_NOT_READY;
    if (!frame || frame->struct_size < sizeof(vt_encoded_frame) || !frame->data || !frame->size ||
        frame->size > s->max_frame_bytes || !frame->width || !frame->height ||
        frame->width > 65535 || frame->height > 65535 || s->peer_address.empty())
        return VT_INVALID_ARGUMENT;
    const size_t count = (frame->size + kPayload - 1) / kPayload;
    if (count > 65535) return VT_INVALID_ARGUMENT;
    std::array<uint8_t, kDatagram> packet{};
    std::memcpy(packet.data(), "VTR1", 4);
    packet[4] = 1;
    packet[5] = frame->flags & 3;
    Put16(packet.data() + 6, count);
    Put32(packet.data() + 12, frame->size);
    Put16(packet.data() + 16, frame->width);
    Put16(packet.data() + 18, frame->height);
    Put64(packet.data() + 20, frame->frame_number);
    Put64(packet.data() + 28, frame->pts_us);
    for (size_t i = 0; i < count; ++i) {
        Put16(packet.data() + 8, i);
        const size_t offset = i * kPayload;
        const size_t length = std::min(kPayload, frame->size - offset);
        std::memcpy(packet.data() + kHeader, frame->data + offset, length);
        if (sendto(s->fd, packet.data(), kHeader + length, 0,
                   reinterpret_cast<const sockaddr*>(&s->peer), sizeof(s->peer)) !=
            static_cast<ssize_t>(kHeader + length)) return VT_IO_ERROR;
        if (i + 1 < count) std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    return VT_OK;
}
void Stop(void* context) {
    auto* s = static_cast<ReferenceUdp*>(context);
    s->running = false;
    if (s->receiver.joinable()) s->receiver.join();
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
    s->assemblies.clear();
}
void Destroy(void* context) { delete static_cast<ReferenceUdp*>(context); }
}  // namespace

extern "C" vt_status vt_reference_udp_create(const vt_reference_udp_config* config,
                                                vt_transport_backend* out) {
    if (!config || !out || config->struct_size < sizeof(vt_reference_udp_config) ||
        !config->bind_address || !config->port || !config->max_frame_bytes ||
        config->max_frame_bytes > 64 * 1024 * 1024) return VT_INVALID_ARGUMENT;
    try {
        auto s = std::make_unique<ReferenceUdp>();
        s->bind_address = config->bind_address;
        s->peer_address = config->peer_address ? config->peer_address : "";
        s->port = config->port;
        s->max_frame_bytes = config->max_frame_bytes;
        *out = {sizeof(vt_transport_backend), VT_ABI_VERSION, s.release(), Start, Send, Stop, Destroy};
        return VT_OK;
    } catch (...) { return VT_IO_ERROR; }
}
