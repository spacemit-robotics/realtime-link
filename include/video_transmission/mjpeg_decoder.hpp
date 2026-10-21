#ifndef INCLUDE_VIDEO_TRANSMISSION_MJPEG_DECODER_HPP  // NOLINT(build/header_guard)
#define INCLUDE_VIDEO_TRANSMISSION_MJPEG_DECODER_HPP  // NOLINT(build/header_guard)

#include <memory>
#include <video_transmission/mpp_frame.hpp>

namespace video_transmission {

// 显式 MJPEG -> NV12 转换，不参与 FpvSession 的输入模式选择。
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

#endif  // INCLUDE_VIDEO_TRANSMISSION_MJPEG_DECODER_HPP
