#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <map>
#include <stdexcept>
#include <video_transmission/session.hpp>
namespace video_transmission {
enum class Role { Sky, Ground };
enum class InputMode { Raw, Encoded };
struct Connection {
    std::string device, device_ip, module_ip, connection_id;
};
struct Connections {
    Connection sky, ground;
};
struct Binding {
    std::string interface, netns, ffs_root;
};
struct PipelineConfig {
    uint32_t width = 640, height = 400, fps = 30, bitrate = 4000000, gop = 30;
    uint32_t roi_x = 0, roi_y = 0, roi_width = 0, roi_height = 0;
    int encoder_id = 0, decoder_id = 0;
    size_t max_frames = 4, max_bytes = 32 * 1024 * 1024;
    uint32_t max_age_ms = 150;
    bool decoded_output = true;
    // 仅 Sky/Raw：H.265 CBR 的 QP 范围，不是固定 QP；省略时保持旧默认值。
    // 0 <= min_qp <= max_qp <= 51，max_qp > 0（MPP 将 0/0 视为不设置）。
    uint32_t min_qp = 10, max_qp = 48;
};
struct VideoTransmissionConfig {
    Role role = Role::Sky;
    InputMode input_mode = InputMode::Raw;
    uint32_t channel = 0;  // 单个会话绑定的协议通道（0 或 1）。
    std::string local_device, profile_path;
    Connections connections;
    std::map<std::string, Binding> bindings;
    SessionConfig link;
    PipelineConfig pipeline;
    bool control_enabled = true;
    uint16_t control_port = 8080, module_port = 8888;
};
// Throws invalid_argument with context. File references are resolved at their declaring file.
VideoTransmissionConfig LoadConfig(const std::string& path);
void ValidateConfig(const VideoTransmissionConfig&);
SessionConfig ResolveLink(const VideoTransmissionConfig&);
void RequireRole(const VideoTransmissionConfig&, Role);
std::string DescribeConfig(const VideoTransmissionConfig&);
}  // namespace video_transmission

#endif  // CONFIG_HPP
