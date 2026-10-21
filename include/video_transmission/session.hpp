#ifndef SESSION_HPP
#define SESSION_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

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

#endif  // SESSION_HPP
