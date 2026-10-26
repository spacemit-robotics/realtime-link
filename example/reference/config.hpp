#ifndef VIDEO_TRANSMISSION_REFERENCE_CONFIG_HPP_
#define VIDEO_TRANSMISSION_REFERENCE_CONFIG_HPP_

#include <string>
#include <video_transmission/video_transmission.hpp>

struct ReferenceConfig {
    vt_session_config session{sizeof(vt_session_config), VT_ABI_VERSION, VT_SKY,
                              VT_INPUT_ENCODED, 8, 8 * 1024 * 1024, 4 * 1024 * 1024, 1000};
    std::string bind_address = "127.0.0.1";
    std::string peer_address = "127.0.0.1";
    uint16_t port = 5006;
    uint32_t width = 640, height = 400;
    vt_reference_udp_config Transport() const {
        return {sizeof(vt_reference_udp_config), bind_address.c_str(), peer_address.c_str(),
                port, session.max_frame_bytes};
    }
};

ReferenceConfig LoadReferenceConfig(const std::string& path, vt_role expected);

#endif
