#include <arpa/inet.h>
#include <yaml-cpp/yaml.h>

#include <cmath>
#include <filesystem>  // NOLINT(build/c++17)
#include <set>
#include <sstream>
#include <video_transmission/config.hpp>
namespace video_transmission {
namespace {
namespace fs = std::filesystem;
void Keys(const YAML::Node& n, std::initializer_list<const char*> allowed, const char* ctx) {
    if (!n || !n.IsMap()) throw std::invalid_argument(std::string(ctx) + ": expected map");
    std::set<std::string> seen;
    for (const auto& x : n) {
        auto key = x.first.as<std::string>();
        bool ok = false;
        for (auto a : allowed)
            if (key == a) ok = true;
        if (!ok || !seen.insert(key).second)
            throw std::invalid_argument(std::string(ctx) + ": unknown/duplicate field " + key);
    }
}
template <class T>
void Read(const YAML::Node& n, const char* k, T& v) {
    if (n[k]) v = n[k].as<T>();
}
std::string Path(const fs::path& base, const std::string& path) {
    if (path.starts_with("package://"))
        throw std::invalid_argument("package:// must be resolved by ROS adapter");
    return (base / fs::path(path)).lexically_normal().string();
}
Connection Conn(const YAML::Node& n) {
    Keys(n, {"device", "device_ip", "module_ip", "connection_id"}, "connection");
    Connection c;
    Read(n, "device", c.device);
    Read(n, "device_ip", c.device_ip);
    Read(n, "module_ip", c.module_ip);
    Read(n, "connection_id", c.connection_id);
    return c;
}
bool IP(const std::string& s) {
    in_addr a{};
    return inet_pton(AF_INET, s.c_str(), &a) == 1 && a.s_addr != INADDR_ANY;
}
}  // namespace
FpvConfig LoadConfig(const std::string& path) {
    try {
        auto n = YAML::LoadFile(path);
        Keys(n,
            {"schema_version", "deployment_profile", "role", "local_device", "camera",
                "connections", "input_mode", "channel", "connections_file", "output"},
            "config");
        if (!n["schema_version"] || n["schema_version"].as<int>() != 2)
            throw std::invalid_argument(
                "schema_version must be 2; migrate legacy config explicitly");
        FpvConfig c;
        auto role = n["role"].as<std::string>();
        if (role != "sky" && role != "ground")
            throw std::invalid_argument("role must be sky or ground");
        c.role = role == "sky" ? Role::Sky : Role::Ground;
        c.local_device = n["local_device"].as<std::string>();
        fs::path base = fs::absolute(path).parent_path();
        c.profile_path = Path(base,
            n["deployment_profile"] ? n["deployment_profile"].as<std::string>()
                                    : "profiles/default.yaml");
        auto p = YAML::LoadFile(c.profile_path);
        Keys(p,
            {"schema_version", "transport", "port", "control_port", "control_enabled",
                "module_port", "rate_mbps", "bindings", "camera", "pipeline", "input_mode",
                "channel"},
            "deployment_profile");
        if (p["schema_version"].as<int>() != 1)
            throw std::invalid_argument("deployment profile version must be 1");
        auto t = p["transport"].as<std::string>();
        if (t != "udp" && t != "usb") throw std::invalid_argument("unsupported transport");
        c.link.transport = t == "udp" ? Transport::Udp : Transport::Usb;
        Read(p, "port", c.link.port);
        Read(p, "rate_mbps", c.link.rate_mbps);
        Read(p, "control_port", c.control_port);
        Read(p, "control_enabled", c.control_enabled);
        Read(p, "module_port", c.module_port);
        if (!p["bindings"] || !p["bindings"].IsMap())
            throw std::invalid_argument("bindings required");
        for (auto b : p["bindings"]) {
            Keys(b.second, {"interface", "netns", "ffs_root"}, "binding");
            Binding v;
            Read(b.second, "interface", v.interface);
            Read(b.second, "netns", v.netns);
            Read(b.second, "ffs_root", v.ffs_root);
            if (!v.netns.empty()) v.netns = Path(fs::path(c.profile_path).parent_path(), v.netns);
            if (!v.ffs_root.empty())
                v.ffs_root = Path(fs::path(c.profile_path).parent_path(), v.ffs_root);
            if (!c.bindings.emplace(b.first.as<std::string>(), v).second)
                throw std::invalid_argument("duplicate binding");
        }
        // 旧 YAML 的 camera 节点仅由适配层读取，SDK 不解释设备字段。
        std::string input_mode = "raw";
        Read(p, "input_mode", input_mode);
        Read(n, "input_mode", input_mode);
        if (input_mode != "raw" && input_mode != "encoded")
            throw std::invalid_argument("input_mode must be raw or encoded");
        c.input_mode = input_mode == "raw" ? InputMode::Raw : InputMode::Encoded;
        Read(p, "channel", c.channel);
        Read(n, "channel", c.channel);
        if (auto a = p["pipeline"]) {
            Keys(a,
                {"width", "height", "fps", "bitrate", "gop", "roi_x", "roi_y", "roi_width",
                    "roi_height", "encoder_id", "decoder_id", "max_frames", "max_bytes",
                    "max_age_ms", "min_qp", "max_qp"},
                "pipeline");
#define R(k) Read(a, #k, c.pipeline.k)
            R(width);
            R(height);
            R(fps);
            R(bitrate);
            R(gop);
            R(min_qp);
            R(max_qp);
            R(roi_x);
            R(roi_y);
            R(roi_width);
            R(roi_height);
            R(encoder_id);
            R(decoder_id);
            R(max_frames);
            R(max_bytes);
            R(max_age_ms);
#undef R
        }
        if (auto o = n["output"]) {
            Keys(o, {"kind"}, "output");
            auto kind = o["kind"].as<std::string>();
            if (kind != "encoded_callback" && kind != "decoded_callback")
                throw std::invalid_argument("unsupported output kind");
            c.pipeline.decoded_output = kind == "decoded_callback";
        }
        if (bool(n["connections"]) == bool(n["connections_file"]))
            throw std::invalid_argument("exactly one connections or connections_file required");
        YAML::Node connections = n["connections"];
        if (n["connections_file"]) {
            auto file = YAML::LoadFile(Path(base, n["connections_file"].as<std::string>()));
            Keys(file, {"connections"}, "connections file");
            connections = file["connections"];
        }
        Keys(connections, {"sky", "ground"}, "connections");
        c.connections = {Conn(connections["sky"]), Conn(connections["ground"])};
        ValidateConfig(c);
        return c;
    } catch (const YAML::Exception& e) {
        throw std::invalid_argument(path + ": " + e.what());
    }
}
void RequireRole(const FpvConfig& c, Role role) {
    if (c.role != role) throw std::invalid_argument("entry point role mismatch");
}
void ValidateConfig(const FpvConfig& c) {
    if ((c.role != Role::Sky && c.role != Role::Ground) ||
        (c.input_mode != InputMode::Raw && c.input_mode != InputMode::Encoded) || c.channel > 1)
        throw std::invalid_argument("invalid role/input mode/channel");
    auto& local = c.role == Role::Sky ? c.connections.sky : c.connections.ground;
    for (auto* p : {&c.connections.sky, &c.connections.ground})
        if (p->device.empty() || !IP(p->device_ip) || !IP(p->module_ip))
            throw std::invalid_argument("connection requires device and valid non-wildcard IPv4");
    if (c.local_device.empty() || c.local_device != local.device)
        throw std::invalid_argument("role does not belong to local_device");
    if (c.bindings.find(local.connection_id) == c.bindings.end())
        throw std::invalid_argument("local connection_id has no binding");
    if (!c.link.port || !c.control_port || !c.module_port || !std::isfinite(c.link.rate_mbps) ||
        c.link.rate_mbps <= 0)
        throw std::invalid_argument("invalid ports/rate");
    auto& b = c.bindings.at(local.connection_id);
    if (c.link.transport == Transport::Usb && b.ffs_root.empty())
        throw std::invalid_argument("USB binding requires ffs_root");
    if (c.connections.sky.device == c.connections.ground.device) {
        if (c.connections.sky.connection_id == c.connections.ground.connection_id)
            throw std::invalid_argument("dual modules require distinct connections");
        auto other = c.bindings.find(c.role == Role::Sky ? c.connections.ground.connection_id
                                                        : c.connections.sky.connection_id);
        if (other == c.bindings.end()) throw std::invalid_argument("dual-device bindings missing");
        if (c.link.transport == Transport::Udp &&
            (b.netns.empty() || other->second.netns.empty() || b.netns == other->second.netns))
            throw std::invalid_argument("same-device UDP requires distinct prepared namespaces");
        if (c.link.transport == Transport::Usb && b.ffs_root == other->second.ffs_root)
            throw std::invalid_argument("USB FunctionFS roots must be distinct");
    }
    auto& p = c.pipeline;
    if (!p.max_frames || p.max_frames > 32 || !p.max_bytes || p.max_bytes > 1024ULL * 1024 * 1024 ||
        !p.max_age_ms || p.max_age_ms > 10000)
        throw std::invalid_argument("invalid pipeline queue budget");
    if (c.role == Role::Sky && c.input_mode == InputMode::Raw) {
        if (!p.max_qp || p.max_qp > 51 || p.min_qp > p.max_qp)
            throw std::invalid_argument(
                "invalid encoder QP range: require 0 <= min_qp <= max_qp <= 51, max_qp > 0");
        if (!p.width || !p.height || p.width > 16384 || p.height > 16384 || p.width % 2 ||
            p.height % 2 || !p.fps || p.fps > 240 || !p.bitrate || !p.gop || p.encoder_id < 0 ||
            p.encoder_id >= 16)
            throw std::invalid_argument("invalid encoder geometry/channel");
        if ((!p.roi_width && (p.roi_x || p.roi_y)) || (p.roi_width == 0) != (p.roi_height == 0) ||
            p.roi_x > 16384 || p.roi_y > 16384 || p.roi_width > 16384 - p.roi_x ||
            p.roi_height > 16384 - p.roi_y || p.roi_x % 2 || p.roi_y % 2 || p.roi_width % 2 ||
            p.roi_height % 2)
            throw std::invalid_argument("invalid ROI");
    }
    if (c.role == Role::Ground && p.decoded_output && (p.decoder_id < 0 || p.decoder_id >= 64))
        throw std::invalid_argument("invalid decoder channel");
}
SessionConfig ResolveLink(const FpvConfig& c) {
    ValidateConfig(c);
    auto l = c.link;
    auto& local = c.role == Role::Sky ? c.connections.sky : c.connections.ground;
    auto& b = c.bindings.at(local.connection_id);
    l.local_address = local.device_ip;
    l.interface = b.interface;
    l.netns = b.netns;
    l.ffs_root = b.ffs_root;
    l.address = c.connections.ground.device_ip;
    if (c.role == Role::Ground) l.peer = c.connections.sky.device_ip;
    return l;
}
std::string DescribeConfig(const FpvConfig& c) {
    auto l = ResolveLink(c);
    std::ostringstream s;
    s << "role=" << (c.role == Role::Sky ? "sky" : "ground") << " profile=" << c.profile_path
        << " transport=" << (l.transport == Transport::Udp ? "udp" : "usb")
        << " source=" << l.local_address << " target=" << l.address << ":" << l.port
        << " interface=" << l.interface << " netns=" << l.netns
        << " input_mode=" << (c.input_mode == InputMode::Raw ? "raw" : "encoded")
        << " channel=" << c.channel;
    // 以下字段是延迟归因的前提：限速、包长、发送队列上限、编码参数与控制端口。
    // 缺失时无法判断发送积压来自节流、调度还是下游承载能力。
    s << " rate_mbps=" << l.rate_mbps << " packet_size_bytes=" << l.packet_size_bytes
        << " limits_frames=" << l.limits.max_frames << " limits_bytes=" << l.limits.max_bytes
        << " limits_frame_bytes=" << l.limits.max_frame_bytes
        << " limits_age_ms=" << l.limits.max_age.count() << " pipeline=" << c.pipeline.width << "x"
        << c.pipeline.height << "@" << c.pipeline.fps << " bitrate=" << c.pipeline.bitrate
        << " gop=" << c.pipeline.gop << " roi=" << c.pipeline.roi_x << "+" << c.pipeline.roi_y << "+"
        << c.pipeline.roi_width << "+" << c.pipeline.roi_height
        << " encoder_id=" << c.pipeline.encoder_id << " decoder_id=" << c.pipeline.decoder_id
        << " pipeline_max_frames=" << c.pipeline.max_frames
        << " pipeline_max_bytes=" << c.pipeline.max_bytes
        << " pipeline_max_age_ms=" << c.pipeline.max_age_ms
        << " control=" << (c.control_enabled ? "on" : "off") << ":" << c.control_port
        << " module_port=" << c.module_port;
    if (c.role == Role::Sky && c.input_mode == InputMode::Raw)
        s << " output=" << c.pipeline.width << "x" << c.pipeline.height
            << " qp=" << c.pipeline.min_qp << ".." << c.pipeline.max_qp;
    return s.str();
}
}  // namespace video_transmission
