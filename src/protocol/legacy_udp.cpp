#include "legacy_udp.hpp"

namespace video_transmission::legacy_udp {
namespace {
void Put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (i * 8));
}
uint32_t Get32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void Put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
uint16_t Get16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
}  // namespace

std::array<uint8_t, kHeaderBytes> Encode(const Header& h) {
    std::array<uint8_t, kHeaderBytes> p{};
    Put32(p.data(), kMagic);
    Put32(p.data() + 4, h.width);
    Put32(p.data() + 8, h.height);
    Put32(p.data() + 16, static_cast<uint32_t>(h.timestamp_us));
    Put32(p.data() + 20, static_cast<uint32_t>(h.timestamp_us >> 32));
    Put32(p.data() + 24, static_cast<uint32_t>(h.frame_number));
    Put32(p.data() + 28, h.frame_len);
    p[32] = h.frame_type;
    p[33] = h.end_of_frame;
    Put16(p.data() + 34, h.packet_seq);
    Put16(p.data() + 36, h.total_packets);
    Put32(p.data() + 52, 3);  // info_version
    Put32(p.data() + 56, h.channel);
    Put32(p.data() + 60, kHeaderBytes);
    Put32(p.data() + 64, h.channel == 1 ? 1 : 0);  // stream_type
    Put32(p.data() + 68, 1);                       // H265
    Put32(p.data() + 72, h.channel == 1 ? 1 : 0);  // stream_id
    Put32(p.data() + 80, static_cast<uint32_t>(h.frame_number >> 32));
    Put32(p.data() + 84, static_cast<uint32_t>(h.timestamp_us));
    Put32(p.data() + 88, static_cast<uint32_t>(h.timestamp_us >> 32));
    Put32(p.data() + 92, h.flags);
    Put32(p.data() + 96, h.slice_index);
    Put32(p.data() + 100, h.slice_count);
    return p;
}

bool Decode(const uint8_t* p, size_t size, Header* h) {
    if (!p || !h || size < kHeaderBytes || Get32(p) != kMagic) return false;
    h->width = Get32(p + 4);
    h->height = Get32(p + 8);
    h->timestamp_us = uint64_t(Get32(p + 16)) | (uint64_t(Get32(p + 20)) << 32);
    h->frame_number = uint64_t(Get32(p + 24));
    h->frame_len = Get32(p + 28);
    h->frame_type = p[32];
    h->end_of_frame = p[33];
    h->packet_seq = Get16(p + 34);
    h->total_packets = Get16(p + 36);
    const uint32_t version = Get32(p + 52);
    if (version > 3 || (version >= 2 && (Get32(p + 60) != kHeaderBytes || Get32(p + 68) != 1)))
        return false;
    if (version >= 2) h->frame_number |= uint64_t(Get32(p + 80)) << 32;
    h->channel = version >= 2 ? Get32(p + 56) : 0;
    h->flags = version >= 2 ? Get32(p + 92) : 0;
    h->slice_index = version >= 3 ? Get32(p + 96) : 0;
    h->slice_count = version >= 3 && Get32(p + 100) > 0 ? Get32(p + 100) : 1;
    return h->width > 0 && h->height > 0 && h->total_packets > 0 &&
        h->packet_seq < h->total_packets && h->frame_len > 0;
}

}  // namespace video_transmission::legacy_udp
