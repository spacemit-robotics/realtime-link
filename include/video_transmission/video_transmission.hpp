#ifndef VIDEO_TRANSMISSION_VIDEO_TRANSMISSION_HPP_
#define VIDEO_TRANSMISSION_VIDEO_TRANSMISSION_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef VIDEO_TRANSMISSION_HAS_PIPELINE
#define VIDEO_TRANSMISSION_HAS_PIPELINE 0
#endif

#if VIDEO_TRANSMISSION_HAS_PIPELINE
#include <sys/vb_type.h>
#endif

namespace video_transmission {

enum class Codec { H265 = 1 };
enum class Transport { Udp, Usb };
enum class SubmitResult { Accepted, QueueFull, NotReady, InvalidFrame };
enum class Event { WaitingForKeyframe, StreamInterrupted, StreamResumed, InvalidPacket };

// 完整 H.265 access unit（AU）。data 为借用：帧进入 SkySession 队列后由发送线程
// 异步消费，调用方必须保证 data 指向的内存存活到发送完成（shared_ptr 的 deleter
// 在最后一个引用释放时触发）。SDK 仅校验元数据边界（session_common.hpp ValidFrame），
// 不解析码流语法。
struct EncodedFrame {
    std::shared_ptr<const uint8_t> data;  // 非空；借用，不拷贝
    size_t size = 0;                      // 字节数：1..max_frame_bytes 且 ≤ uint32 上限
    Codec codec = Codec::H265;            // 必须 Codec::H265
    uint32_t width = 0;                   // 非零且 ≤ 16384
    uint32_t height = 0;                  // 非零且 ≤ 16384
    uint32_t channel = 0;                 // 0 或 1，须等于配置 channel
    uint64_t frame_number = 0;
    uint64_t timestamp_us = 0;    // PTS，用于延迟测量与组包时间戳
    uint64_t generation = 0;      // Local connection epoch; never changes the legacy wire format.
                                  // 置 0 提交时绑定当前代；非零须与会话当前代一致
    bool keyframe = false;        // 真实 IDR 标记；复位/重连/队列溢出后非关键帧会被拒收
    bool parameter_sets = false;  // 该 AU 含 VPS/SPS/PPS 时置位，对端据此识别参数集
};

struct Limits {
    size_t max_frames = 16;
    size_t max_bytes = 16 * 1024 * 1024;
    size_t max_frame_bytes = 10 * 1024 * 1024;
    // 额外拥塞等待上限。UDP 入队时另计前方所有待发帧的理论发送时间，
    // 帧数/字节数仍有界；USB 和接收后的回调排队仍按原始等待时间计。
    // UDP 帧发送/组帧另加按帧长和 rate_mbps 计算的传输时间。
    // 截止时间固定，不因后续流量或调度延迟不断延长。
    std::chrono::milliseconds max_age{200};
};

struct SessionConfig {
    Transport transport = Transport::Udp;
    std::string address;        // UDP destination for sky, listen address for ground.
    std::string local_address;  // Explicit sky source binding.
    std::string interface;      // Optional SO_BINDTODEVICE.
    std::string netns;          // Prepared namespace path; opened only on a dedicated thread.
    std::string peer;           // Optional source IPv4 filter for ground.
    std::string ffs_root = "/dev/usb-ffs/customffs";
    uint16_t port = 5005;
    uint32_t packet_size_bytes = 1400;
    double rate_mbps = 12.0;
    Limits limits;
    uint32_t channel = 0;       // Accepted video protocol channel (0 or 1).
};

class SkySession {
public:
    // Start/Stop are externally serialized; callbacks must not call blocking Stop.
    SkySession();
    ~SkySession();
    SkySession(const SkySession&) = delete;
    SkySession& operator=(const SkySession&) = delete;
    bool Start(const SessionConfig& config);
    SubmitResult SubmitEncodedFrame(EncodedFrame frame);
    void StopSending();
    bool Ready() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class GroundSession {
public:
    // Configure callbacks before Start; Stop waits for callback completion.
    GroundSession();
    ~GroundSession();
    GroundSession(const GroundSession&) = delete;
    GroundSession& operator=(const GroundSession&) = delete;
    bool Start(const SessionConfig& config);
    void OnEncodedFrame(std::function<void(EncodedFrame)> callback);
    void OnEvent(std::function<void(Event)> callback);
    void StopReceiving();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace video_transmission

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

namespace video_transmission {
struct ControlMessage {
    uint32_t tag = 0;
    std::vector<uint8_t> payload;
};
enum class ControlStatus { Applied, Rejected, NotSent, Unknown, Cancelled, QueueFull };
struct ControlReply {
    ControlStatus status = ControlStatus::NotSent;
    ControlMessage message;
};
class ControlSession {
public:
    ControlSession();
    ~ControlSession();
    bool Start(const VideoTransmissionConfig&, std::function<ControlMessage(const ControlMessage&)> handler = {});
    std::future<ControlReply> SendControl(
        ControlMessage, std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Explicit module management call; credentials are supplied by the caller and never logged.
ControlReply ExecuteModuleAt(const VideoTransmissionConfig&, const std::string& command,
    const std::string& user = {}, const std::string& password = {},
    std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
}  // namespace video_transmission

namespace video_transmission::latency {
// All timestamps are local CLOCK_MONOTONIC microseconds. PTS is an identity,
// never a ground-local clock. Cross-host subtraction requires clock calibration.
uint64_t NowUs();
bool Enabled();
// Set VIDEO_TRANSMISSION_TRACE_PATH before starting the process. A run must use a new file.
// value carries event-specific metadata (wire sequence, bytes, presentation flags).
// note 是可选诊断文本，写入 CSV 第五列；字段内部用 ';' 分隔，不得含逗号、CR、
// LF（写入前会被替换为 '_'，并截断到 192 字节）。该列只承载诊断信息，不参与
// 任何阶段计算，省略时为空。
void Mark(const char* event, uint64_t pts, uint64_t value = 0, uint64_t at_us = 0,
    const char* note = nullptr);
}  // namespace video_transmission::latency

namespace video_transmission {
// 构建身份：库版本、源码修订、构建时间、构建选项与编译器。
// 全部来自构建期注入的宏，不做系统调用；返回值进程内只构造一次。
// 文本不含逗号与空白，可安全写入日志行和 trace CSV 的 note 列。
const std::string& BuildInfo();
// 源码修订：<短修订号>（工作区含未提交修改时追加 -dirty）；取不到时为 unknown。
const std::string& SourceRevision();
}  // namespace video_transmission

#if VIDEO_TRANSMISSION_HAS_PIPELINE
namespace video_transmission {

// owner 必须保持本进程合法 MPP VB、设备和 runtime 到最后一次释放。
// release 可能在提交/媒体/回调/停止线程执行；有线程约束的 provider 自行投递释放。
// generation 为 0 时提交绑定当前代际；非零值必须匹配会话。
struct MppFrame {
    VideoFrameInfo info{};
    std::shared_ptr<const void> owner;
    uint32_t channel = 0;
    uint64_t frame_number = 0;
    uint64_t generation = 0;

    explicit operator bool() const { return static_cast<bool>(owner); }
};

class MppRuntime {
public:
    // 获取/归还底层 SYS、VB 的配对引用；调用方需串行化进程内 runtime 生命周期操作，
    // 当前 MPP 的底层 init 计数本身不具备并发保护。
    MppRuntime();
    ~MppRuntime();
    MppRuntime(const MppRuntime&) = delete;
    MppRuntime& operator=(const MppRuntime&) = delete;
};

// 从调用开始接管释放责任，包括内部内存分配失败；release 必须不抛异常。
MppFrame AdoptMppFrame(VideoFrameInfo info, std::function<void(const VideoFrameInfo&)> release,
    std::shared_ptr<void> context = {}, uint32_t channel = 0, uint64_t frame_number = 0,
    uint64_t generation = 0);
// 校验线性 NV12/BGRA 与本进程 VB 布局；MJPEG 应先由独立 helper 解码。
// 失败抛 invalid_argument，不初始化 runtime。
void ValidateMppFrame(const MppFrame& frame);

}  // namespace video_transmission

namespace video_transmission {

// 显式 MJPEG -> NV12 转换，不参与 VideoTransmission 的输入模式选择。
// 输入必须为本进程 MPP VB 的单平面 MJPEG，owner 保持映射及 runtime；
// 输出租约保持 VDEC 通道和 runtime，可超过 helper 的生命周期。
class MjpegDecoder {
public:
    MjpegDecoder(int decoder_id, uint32_t width, uint32_t height);
    ~MjpegDecoder();
    MjpegDecoder(const MjpegDecoder&) = delete;
    MjpegDecoder& operator=(const MjpegDecoder&) = delete;

    // 串行解码，保留输入 channel/frame_number/generation；失败后需重建 helper。
    // 校验错误抛 invalid_argument，设备/同步/迟到输出错误抛 runtime_error。
    MppFrame Decode(const MppFrame& input);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace video_transmission

namespace video_transmission {
enum class StatusCode { Ok, InvalidConfig, NotReady, Busy, RestartRequired, Cancelled, Failed };
struct Result {
    StatusCode code = StatusCode::Ok;
    std::string message;
    uint64_t generation = 0;
    bool ok() const { return code == StatusCode::Ok; }
};
using DecodedFrame = MppFrame;
class VideoTransmission {
public:
    VideoTransmission();
    ~VideoTransmission();
    VideoTransmission(const VideoTransmission&) = delete;
    VideoTransmission& operator=(const VideoTransmission&) = delete;
    // Start 前注册。状态/视频回调在 SDK dispatch 线程执行。
    // 回调中提交或 SendControl 遇生命周期锁占用时立即返回未就绪，不能阻塞等待重试。
    void OnState(std::function<void(Result)>);
    void OnEncodedFrame(std::function<void(EncodedFrame)>);
    void OnVideoFrame(std::function<void(MppFrame)>);
    Result Start(const VideoTransmissionConfig&);
    // 仅 Sky/Raw：带 owner 的线性 NV12 或 BGRA；首帧确定编码输入格式，切换须重启。
    // NV12 支持 ROI/缩放；BGRA 要求完整同尺寸、width*4 stride，直接送 VENC，不做 V2D。
    // BGRA 宽度须为 16 的倍数；提交前生产者须完成 DMA 写入，owner 释放前不可修改像素。
    // BGRA 输入 owner 保持至对应编码输出或编码器关闭；在途最多 max_frames+3 帧。
    // 队列淘汰或编码端忙时可丢帧，Accepted 仅表示进入异步队列，不保证最终编码。
    // 两种提交均要求 channel 等于配置；generation=0 绑定当前代，非零不匹配则拒绝。
    SubmitResult SubmitFrame(MppFrame);
    // 仅 Sky/Encoded：提交一个完整 H.265 access unit，直接交给 Sky 发送，不创建 MPP
    // 编码资源。data 为借用，调用方须保证内存存活到发送完成；含 VPS/SPS/PPS 的 AU 置
    // parameter_sets=true，IDR 帧置 keyframe=true。仅校验元数据边界（session_common.hpp
    // ValidFrame），不解析码流语法，错误内容会原样上链路。
    SubmitResult SubmitEncodedFrame(EncodedFrame);
    std::future<Result> UpdateConnections(Connections);
    std::future<ControlReply> SendControl(ControlMessage);
    // 保留已有控制接口：远端请求在控制线程调用，本地外部编码器 IDR 恢复在 dispatch
    // 线程调用（tag=0xa00e）；两者可能并发，处理器需自行同步，且不能阻塞等待 SDK。
    void OnCameraControl(std::function<ControlMessage(const ControlMessage&)>);
    // 回调内禁止 Stop/UpdateConnections；已交付帧的 owner 在 Stop 后仍有效。
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace video_transmission

#endif  // VIDEO_TRANSMISSION_HAS_PIPELINE

#endif  // VIDEO_TRANSMISSION_VIDEO_TRANSMISSION_HPP_
