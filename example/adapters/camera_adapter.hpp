#ifndef CAMERA_ADAPTER_HPP
#define CAMERA_ADAPTER_HPP

#include <chrono>
#include <memory>

namespace video_transmission {
struct MppFrame;
struct ControlMessage;
}  // namespace video_transmission

namespace video_transmission::example {
struct ExampleConfig;

/** 示例采集器；Get 由生产线程串行调用，停止生产后再停止 session。 */
class CameraAdapter {
public:
    explicit CameraAdapter(const ExampleConfig& config);
    ~CameraAdapter();
    CameraAdapter(const CameraAdapter&) = delete;
    CameraAdapter& operator=(const CameraAdapter&) = delete;
    MppFrame Get(std::chrono::milliseconds timeout);
    /** 停止取得新帧，已交付帧继续持有设备与 runtime。 */
    void Stop();
    /** 未定义的设备命令明确返回协议错误，不猜测 payload 布局。 */
    ControlMessage HandleControl(const ControlMessage& message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace video_transmission::example

#endif  // CAMERA_ADAPTER_HPP
