#include "config.hpp"

#include <yaml-cpp/yaml.h>

#include <stdexcept>

ReferenceConfig LoadReferenceConfig(const std::string& path, vt_role expected) {
    const auto root = YAML::LoadFile(path);
    if (!root.IsMap() || !root["schema_version"] || root["schema_version"].as<int>() != 1)
        throw std::invalid_argument("reference config schema_version must be 1");
    const auto role = root["role"].as<std::string>();
    ReferenceConfig c;
    c.session.role = role == "sky" ? VT_SKY : role == "ground" ? VT_GROUND : vt_role(0);
    if (c.session.role != expected) throw std::invalid_argument("reference config role mismatch");
    const auto transport = root["transport"];
    if (!transport.IsMap() || transport["backend"].as<std::string>() != "reference_udp")
        throw std::invalid_argument("reference_udp transport required");
    c.bind_address = transport["bind"].as<std::string>();
    c.peer_address = transport["peer"] ? transport["peer"].as<std::string>() : "";
    c.port = transport["port"].as<uint16_t>();
    if (const auto limits = root["limits"]) {
        if (limits["max_frames"]) c.session.max_frames = limits["max_frames"].as<uint32_t>();
        if (limits["max_bytes"]) c.session.max_bytes = limits["max_bytes"].as<size_t>();
        if (limits["max_frame_bytes"])
            c.session.max_frame_bytes = limits["max_frame_bytes"].as<size_t>();
        if (limits["max_age_ms"]) c.session.max_age_ms = limits["max_age_ms"].as<uint32_t>();
    }
    if (expected == VT_SKY) {
        const auto video = root["video"];
        if (!video.IsMap()) throw std::invalid_argument("sky video width/height required");
        c.width = video["width"].as<uint32_t>();
        c.height = video["height"].as<uint32_t>();
    }
    if (!c.port || !c.session.max_frames || !c.session.max_bytes || !c.session.max_frame_bytes ||
        c.session.max_frame_bytes > c.session.max_bytes || !c.session.max_age_ms ||
        !c.width || !c.height || c.width > 16384 || c.height > 16384)
        throw std::invalid_argument("invalid reference config limits or geometry");
    return c;
}
