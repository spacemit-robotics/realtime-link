#ifndef MPP_FRAME_HPP
#define MPP_FRAME_HPP

#include <sys/vb_type.h>

#include <cstdint>
#include <functional>
#include <memory>

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
// VDEC may omit physical plane addresses from GetFrame; complete them from
// the same local VB handle before validating or exporting a decoded NV12 frame.
void CompleteVdecFramePhysicalAddresses(VideoFrameInfo& info);
// 校验线性 NV12/BGRA 与本进程 VB 布局；MJPEG 应先由独立 helper 解码。
// 失败抛 invalid_argument，不初始化 runtime。
void ValidateMppFrame(const MppFrame& frame);

}  // namespace video_transmission

#endif  // MPP_FRAME_HPP
