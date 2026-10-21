#ifndef LEGACY_UDP_HPP
#define LEGACY_UDP_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace video_transmission::legacy_udp {

constexpr size_t kHeaderBytes = 128;
constexpr size_t kPacketBytes = 1400;
constexpr size_t kPayloadBytes = kPacketBytes - kHeaderBytes;
constexpr uint32_t kMagic = 0xfafafafa;

// Explicit offsets match the K1 FrameHeader ABI, including four bytes of
// padding before timestamp and two after total_packets. Legacy peers memcpy
// this little-endian layout; do not transmit a new C++ struct directly.
struct Header {
    uint32_t width = 0, height = 0;
    uint64_t timestamp_us = 0;
    uint64_t frame_number = 0;
    uint32_t frame_len = 0;
    uint8_t frame_type = 1, end_of_frame = 1;
    uint16_t packet_seq = 0, total_packets = 0;
    uint32_t channel = 0, slice_index = 0, slice_count = 1;
    uint32_t flags = 0;
};

std::array<uint8_t, kHeaderBytes> Encode(const Header& header);
bool Decode(const uint8_t* packet, size_t size, Header* header);

}  // namespace video_transmission::legacy_udp

#endif  // LEGACY_UDP_HPP
