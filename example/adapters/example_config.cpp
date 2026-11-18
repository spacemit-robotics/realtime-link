/** @file example_config.cpp
 *  @brief 示例 YAML 配置解析与采集参数校验，不依赖 SDK 摄像头配置。
 */
#include "example_config.hpp"

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <initializer_list>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace video_transmission::example {
namespace {

namespace fs = std::filesystem;
using video_transmission::Binding;
using video_transmission::Connection;
using video_transmission::VideoTransmissionConfig;
using video_transmission::InputMode;
using video_transmission::Role;
using video_transmission::Transport;

void CheckKeys(
    const YAML::Node& node, std::initializer_list<const char*> allowed, std::string_view context) {
    if (!node || !node.IsMap()) {
        throw std::invalid_argument(std::string(context) + ": expected map");
    }

    std::set<std::string> seen;
    for (const auto& item : node) {
        const auto key = item.first.as<std::string>();
        bool allowed_key = false;
        for (const auto candidate : allowed) {
            if (key == candidate) {
                allowed_key = true;
                break;
            }
        }
        if (!allowed_key || !seen.insert(key).second) {
            throw std::invalid_argument(std::string(context) + ": unknown/duplicate field " + key);
        }
    }
}

template <typename Type>
void Read(const YAML::Node& node, const char* key, Type& value) {
    if (node[key]) {
        value = node[key].as<Type>();
    }
}

std::string ResolvePath(const fs::path& base, const std::string& value) {
    return (base / fs::path(value)).lexically_normal().string();
}

void ReadInputMode(const YAML::Node& node, VideoTransmissionConfig& config, std::string_view context) {
    if (!node) {
        return;
    }
    const auto mode = node.as<std::string>();
    if (mode == "raw") {
        config.input_mode = InputMode::Raw;
    } else if (mode == "encoded") {
        config.input_mode = InputMode::Encoded;
    } else {
        throw std::invalid_argument(std::string(context) + ": input_mode must be raw or encoded");
    }
}

Connection ReadConnection(const YAML::Node& node) {
    CheckKeys(node, {"device", "device_ip", "module_ip", "connection_id"}, "connection");
    Connection connection;
    Read(node, "device", connection.device);
    Read(node, "device_ip", connection.device_ip);
    Read(node, "module_ip", connection.module_ip);
    Read(node, "connection_id", connection.connection_id);
    return connection;
}

void ReadConnections(const YAML::Node& node, VideoTransmissionConfig& config) {
    CheckKeys(node, {"sky", "ground"}, "connections");
    config.connections.sky = ReadConnection(node["sky"]);
    config.connections.ground = ReadConnection(node["ground"]);
}

void ReadBindings(const YAML::Node& node, VideoTransmissionConfig& config, const fs::path& base) {
    if (!node || !node.IsMap()) {
        throw std::invalid_argument("bindings: expected map");
    }
    for (const auto& item : node) {
        const auto name = item.first.as<std::string>();
        CheckKeys(item.second, {"interface", "netns", "ffs_root"}, "binding");
        Binding binding;
        Read(item.second, "interface", binding.interface);
        Read(item.second, "netns", binding.netns);
        Read(item.second, "ffs_root", binding.ffs_root);
        if (!binding.netns.empty()) {
            binding.netns = ResolvePath(base, binding.netns);
        }
        if (!binding.ffs_root.empty()) {
            binding.ffs_root = ResolvePath(base, binding.ffs_root);
        }
        if (!config.bindings.emplace(name, std::move(binding)).second) {
            throw std::invalid_argument("duplicate binding " + name);
        }
    }
}

void ReadCapture(const YAML::Node& node, CaptureConfig& capture, std::string_view context) {
    if (!node) {
        return;
    }
    CheckKeys(node,
        {"driver", "device", "format", "stream", "width", "height", "fps", "depth",
            "device_id", "channel_id", "channel"},
        context);
    Read(node, "driver", capture.driver);
    Read(node, "device", capture.device);
    Read(node, "format", capture.format);
    Read(node, "stream", capture.stream);
    Read(node, "width", capture.width);
    Read(node, "height", capture.height);
    Read(node, "fps", capture.fps);
    Read(node, "depth", capture.depth);
    Read(node, "device_id", capture.device_id);
    Read(node, "channel_id", capture.channel_id);
    Read(node, "channel", capture.channel);
}

void ReadPipeline(const YAML::Node& node, VideoTransmissionConfig& config) {
    if (!node) {
        return;
    }
    CheckKeys(node,
        {"width", "height", "fps", "bitrate", "gop", "roi_x", "roi_y", "roi_width", "roi_height",
            "encoder_id", "decoder_id", "max_frames", "max_bytes", "max_age_ms", "decoded_output"},
        "pipeline");
#define READ_PIPELINE_FIELD(field) Read(node, #field, config.pipeline.field)
    READ_PIPELINE_FIELD(width);
    READ_PIPELINE_FIELD(height);
    READ_PIPELINE_FIELD(fps);
    READ_PIPELINE_FIELD(bitrate);
    READ_PIPELINE_FIELD(gop);
    READ_PIPELINE_FIELD(roi_x);
    READ_PIPELINE_FIELD(roi_y);
    READ_PIPELINE_FIELD(roi_width);
    READ_PIPELINE_FIELD(roi_height);
    READ_PIPELINE_FIELD(encoder_id);
    READ_PIPELINE_FIELD(decoder_id);
    READ_PIPELINE_FIELD(max_frames);
    READ_PIPELINE_FIELD(max_bytes);
    READ_PIPELINE_FIELD(max_age_ms);
    READ_PIPELINE_FIELD(decoded_output);
#undef READ_PIPELINE_FIELD
}

void ReadSessionFields(const YAML::Node& node, VideoTransmissionConfig& config) {
    const auto transport = node["transport"];
    if (transport) {
        const auto value = transport.as<std::string>();
        if (value == "udp") {
            config.link.transport = Transport::Udp;
        } else if (value == "usb") {
            config.link.transport = Transport::Usb;
        } else {
            throw std::invalid_argument("unsupported transport");
        }
    }
    Read(node, "port", config.link.port);
    Read(node, "packet_size_bytes", config.link.packet_size_bytes);
    Read(node, "rate_mbps", config.link.rate_mbps);
    Read(node, "control_port", config.control_port);
    Read(node, "control_enabled", config.control_enabled);
    Read(node, "module_port", config.module_port);
    if (const auto limits = node["limits"]) {
        CheckKeys(limits, {"max_frames", "max_bytes", "max_frame_bytes", "max_age_ms"}, "limits");
        Read(limits, "max_frames", config.link.limits.max_frames);
        Read(limits, "max_bytes", config.link.limits.max_bytes);
        Read(limits, "max_frame_bytes", config.link.limits.max_frame_bytes);
        uint32_t max_age_ms = static_cast<uint32_t>(config.link.limits.max_age.count());
        Read(limits, "max_age_ms", max_age_ms);
        config.link.limits.max_age = std::chrono::milliseconds(max_age_ms);
    }
}

void ReadProfile(const YAML::Node& node, ExampleConfig& config, const fs::path& profile_base) {
    CheckKeys(node,
        {"schema_version", "transport", "port", "packet_size_bytes", "rate_mbps", "control_port",
            "control_enabled", "module_port", "limits", "input_mode", "bindings", "camera",
            "pipeline"},
        "deployment_profile");
    if (!node["schema_version"] || node["schema_version"].as<int>() != 1) {
        throw std::invalid_argument("deployment profile version must be 1");
    }
    if (!node["transport"]) {
        throw std::invalid_argument("deployment_profile: transport required");
    }
    ReadSessionFields(node, config.session);
    ReadInputMode(node["input_mode"], config.session, "deployment_profile");
    ReadBindings(node["bindings"], config.session, profile_base);
    ReadCapture(node["camera"], config.capture, "camera profile");
    ReadPipeline(node["pipeline"], config.session);
}

void CheckRoot(const YAML::Node& node) {
    CheckKeys(node,
        {"schema_version", "deployment_profile", "role", "local_device", "input_mode", "camera",
            "connections", "connections_file", "output"},
        "config");
    if (!node["schema_version"] || node["schema_version"].as<int>() != 2) {
        throw std::invalid_argument("schema_version must be 2");
    }
}

void ReadRoot(const YAML::Node& node, ExampleConfig& config, const fs::path& root_base) {
    const auto role = node["role"].as<std::string>();
    if (role != "sky" && role != "ground") {
        throw std::invalid_argument("role must be sky or ground");
    }
    config.session.role = role == "sky" ? Role::Sky : Role::Ground;
    config.session.local_device = node["local_device"].as<std::string>();
    ReadInputMode(node["input_mode"], config.session, "config");
    ReadCapture(node["camera"], config.capture, "camera override");

    if (const auto output = node["output"]) {
        CheckKeys(output, {"kind"}, "output");
        const auto kind = output["kind"].as<std::string>();
        if (kind != "encoded_callback" && kind != "decoded_callback") {
            throw std::invalid_argument("unsupported output kind");
        }
        config.session.pipeline.decoded_output = kind == "decoded_callback";
    }

    if (bool(node["connections"]) == bool(node["connections_file"])) {
        throw std::invalid_argument("exactly one connections or connections_file required");
    }
    if (node["connections"]) {
        ReadConnections(node["connections"], config.session);
    } else {
        const auto connections_path =
            ResolvePath(root_base, node["connections_file"].as<std::string>());
        const auto connections = YAML::LoadFile(connections_path);
        CheckKeys(connections, {"connections"}, "connections file");
        ReadConnections(connections["connections"], config.session);
    }
}

}  // namespace

ExampleConfig LoadExampleConfig(const fs::path& path) {
    try {
        const auto absolute_path = fs::absolute(path);
        const auto root = YAML::LoadFile(absolute_path.string());
        CheckRoot(root);
        ExampleConfig config;

        const auto profile_path = ResolvePath(absolute_path.parent_path(),
            root["deployment_profile"] ? root["deployment_profile"].as<std::string>()
                                        : "profiles/default.yaml");
        config.session.profile_path = profile_path;
        const auto profile = YAML::LoadFile(profile_path);
        ReadProfile(profile, config, fs::path(profile_path).parent_path());

        // 根配置覆盖 profile，同名字段在各自映射内仍必须唯一。
        ReadRoot(root, config, absolute_path.parent_path());
        ValidateExampleConfig(config);
        return config;
    } catch (const YAML::Exception& error) {
        throw std::invalid_argument(path.string() + ": " + error.what());
    } catch (const std::invalid_argument& error) {
        throw std::invalid_argument(path.string() + ": " + error.what());
    }
}

void ValidateExampleConfig(const ExampleConfig& config) {
    video_transmission::ValidateConfig(config.session);
    if (config.session.role != Role::Sky || config.session.input_mode != InputMode::Raw) {
        return;
    }

    const auto& capture = config.capture;
    const auto& pipeline = config.session.pipeline;
    if (capture.driver.empty() || !capture.width || !capture.height || capture.width > 16384 ||
        capture.height > 16384 || !capture.fps || capture.fps > 240 ||
        capture.fps != pipeline.fps || !capture.depth || capture.device_id < 0 ||
        capture.channel_id < 0) {
        throw std::invalid_argument("invalid raw capture device or geometry");
    }
    if (capture.format != "mjpeg" && capture.format != "nv12") {
        throw std::invalid_argument("capture format must be mjpeg or nv12");
    }
    if (capture.format == "nv12" && (capture.width % 2 || capture.height % 2)) {
        throw std::invalid_argument("NV12 capture requires even width and height");
    }
    // 与当前 MPP 设备接口一致；不包含 MPP 头文件，避免引入设备依赖。
    constexpr int kMaxDeviceCount = 4;
    constexpr int kMaxUvcChannelCount = 1;
    constexpr int kMaxViChannelCount = 6;
    constexpr size_t kDevicePathCapacity = 128;
    if (capture.driver == "mpp_uvc" &&
        (capture.device.empty() || !fs::path(capture.device).is_absolute() ||
            capture.device.find('\0') != std::string::npos ||
            capture.device.size() >= kDevicePathCapacity || capture.device_id >= kMaxDeviceCount ||
            capture.channel_id >= kMaxUvcChannelCount)) {
        throw std::invalid_argument("invalid mpp_uvc device path or device/channel ID");
    }
    if (capture.driver == "mpp_vi" &&
        (capture.format != "nv12" || capture.device_id >= kMaxDeviceCount ||
            capture.channel_id >= kMaxViChannelCount)) {
        throw std::invalid_argument("invalid mpp_vi format or device/channel ID");
    }
    if (capture.driver != "mpp_uvc" && capture.driver != "mpp_vi") {
        throw std::invalid_argument("capture driver must be mpp_uvc or mpp_vi");
    }
    if ((!pipeline.roi_width && (pipeline.roi_x || pipeline.roi_y)) ||
        (pipeline.roi_width == 0) != (pipeline.roi_height == 0) || pipeline.roi_x > capture.width ||
        pipeline.roi_y > capture.height || pipeline.roi_width > capture.width - pipeline.roi_x ||
        pipeline.roi_height > capture.height - pipeline.roi_y || pipeline.roi_x % 2 ||
        pipeline.roi_y % 2 || pipeline.roi_width % 2 || pipeline.roi_height % 2) {
        throw std::invalid_argument("invalid ROI");
    }
}

std::string DescribeExampleConfig(const ExampleConfig& config) {
    const auto link = video_transmission::ResolveLink(config.session);
    std::ostringstream description;
    // 会话部分复用 SDK 的 DescribeConfig：启动日志与 trace 留档必须描述同一组
    // 实际生效参数（限速/包长/队列上限/编码参数），避免两处实现漂移。
    description << video_transmission::DescribeConfig(config.session)
                << " ffs_root=" << link.ffs_root << " capture_driver=" << config.capture.driver
                << " capture_device=" << config.capture.device
                << " capture_format=" << config.capture.format
                << " capture_geometry=" << config.capture.width << "x" << config.capture.height
                << " capture_fps=" << config.capture.fps
                << " capture_channel=" << config.capture.channel
                << " device_id=" << config.capture.device_id
                << " channel_id=" << config.capture.channel_id << " depth=" << config.capture.depth
                << " stream=" << config.capture.stream
                << " output_kind="
                << (config.session.pipeline.decoded_output ? "decoded_callback"
                                                            : "encoded_callback");
    return description.str();
}

}  // namespace video_transmission::example
