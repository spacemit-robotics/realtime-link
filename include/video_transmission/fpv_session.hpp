#ifndef FPV_SESSION_HPP
#define FPV_SESSION_HPP

#include <future>
#include <video_transmission/config.hpp>
#include <video_transmission/control.hpp>
#include <video_transmission/mpp_frame.hpp>
namespace video_transmission {
enum class StatusCode { Ok, InvalidConfig, NotReady, Busy, RestartRequired, Cancelled, Failed };
struct Result {
    StatusCode code = StatusCode::Ok;
    std::string message;
    uint64_t generation = 0;
    bool ok() const { return code == StatusCode::Ok; }
};
using DecodedFrame = MppFrame;
class FpvSession {
public:
    FpvSession();
    ~FpvSession();
    FpvSession(const FpvSession&) = delete;
    FpvSession& operator=(const FpvSession&) = delete;
    // Start 前注册。状态/视频回调在 SDK dispatch 线程执行。
    // 回调中提交或 SendControl 遇生命周期锁占用时立即返回未就绪，不能阻塞等待重试。
    void OnState(std::function<void(Result)>);
    void OnEncodedFrame(std::function<void(EncodedFrame)>);
    void OnVideoFrame(std::function<void(MppFrame)>);
    Result Start(const FpvConfig&);
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

#endif  // FPV_SESSION_HPP
