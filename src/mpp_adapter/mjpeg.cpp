#include <video_transmission/mpp_mjpeg.h>
#include <vdec/vdec_api.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <climits>
#include <memory>
#include <new>
namespace {
struct Decoder {
    int channel;
    uint32_t width, height, output_width, output_height;
    bool created = false, enabled = false, failed = false;
    ~Decoder() {
        if (enabled) VDEC_DisableChn(channel);
        if (created) VDEC_DestroyChn(channel);
    }
};
struct Lease {
    std::shared_ptr<Decoder> decoder;
};
void release_frame(void* user, const VideoFrameInfo* info) {
    std::unique_ptr<Lease> lease(static_cast<Lease*>(user));
    VDEC_ReleaseFrame(lease->decoder->channel, info->ulBufferId);
}
int sync(int fd, uint64_t flags) {
    dma_buf_sync s{flags};
    int rc;
    do {
        rc = ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
    } while (rc < 0 && errno == EINTR);
    return rc;
}
}  // namespace
struct vt_mpp_mjpeg {
    std::shared_ptr<Decoder> decoder;
};
extern "C" VT_API vt_status vt_mpp_mjpeg_open_scaled(
    int channel, uint32_t width, uint32_t height, uint32_t output_width, uint32_t output_height, vt_mpp_mjpeg** out) {
    if (!out) return VT_INVALID_ARGUMENT;
    *out = nullptr;
    if (channel < 0 || channel >= VDEC_MAX_CHN || !width || !height || width > 16384 || height > 16384 ||
        !output_width || !output_height || output_width > 16384 || output_height > 16384)
        return VT_INVALID_ARGUMENT;
    try {
        auto d = std::make_shared<Decoder>();
        d->channel = channel;
        d->width = width;
        d->height = height;
        d->output_width = output_width;
        d->output_height = output_height;
        VdecChnAttr a{};
        a.eCodecType = MPP_STREAM_CODEC_MJPEG;
        a.eOutputMode = VDEC_OUTPUT_PULL;
        a.eOutputPixelFormat = MPP_PIXEL_FORMAT_NV12;
        a.u32Width = width;
        a.u32Height = height;
        if (width != output_width || height != output_height) {
            a.stScale.bScaleEnable = MPP_TRUE;
            a.stScale.u32Width = output_width;
            a.stScale.u32Height = output_height;
        }
        if (VDEC_CreateChn(channel, &a)) return VT_IO_ERROR;
        d->created = true;
        if (VDEC_EnableChn(channel)) return VT_IO_ERROR;
        d->enabled = true;
        *out = new vt_mpp_mjpeg{std::move(d)};
        return VT_OK;
    } catch (...) {
        return VT_IO_ERROR;
    }
}
extern "C" VT_API vt_status vt_mpp_mjpeg_open(int channel, uint32_t width, uint32_t height, vt_mpp_mjpeg** out) {
    return vt_mpp_mjpeg_open_scaled(channel, width, height, width, height, out);
}
extern "C" VT_API vt_status vt_mpp_mjpeg_decode(vt_mpp_mjpeg* self, const vt_raw_frame* input, vt_raw_frame* out) {
    if (!self || !out) return VT_INVALID_ARGUMENT;
    *out = {};
    auto d = self->decoder;
    if (d->failed) return VT_NOT_READY;
    const auto* info = vt_mpp_frame_info(input);
    if (!info || input->pixel_format != MPP_PIXEL_FORMAT_MJPEG || input->width != d->width ||
        input->height != d->height || info->stVFrame.u32PlaneNum != 1 || !info->stVFrame.ulPlaneVirAddr[0] ||
        !info->stVFrame.u32PlaneSizeValid[0] || info->stVFrame.u32PlaneSizeValid[0] > info->stVFrame.u32PlaneSize[0] ||
        info->stVFrame.u32Fd[0] > INT_MAX)
        return VT_INVALID_ARGUMENT;
    const int fd = static_cast<int>(info->stVFrame.u32Fd[0]);
    if (sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ)) {
        d->failed = true;
        return VT_IO_ERROR;
    }
    StreamBufferInfo stream{};
    stream.pu8Addr = reinterpret_cast<const U8*>(info->stVFrame.ulPlaneVirAddr[0]);
    stream.u32Size = info->stVFrame.u32PlaneSizeValid[0];
    stream.u64PTS = input->pts_us;
    stream.eCodecType = MPP_STREAM_CODEC_MJPEG;
    const int send_result = VDEC_SendStream(d->channel, &stream, 100);
    const int sync_result = sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    if (send_result || sync_result) {
        d->failed = true;
        return VT_IO_ERROR;
    }
    VideoFrameInfo output{};
    if (VDEC_GetFrame(d->channel, &output, 1000)) {
        d->failed = true;
        return VT_IO_ERROR;
    }
    if (output.stVFrame.u64PTS != input->pts_us || output.stCommFrameInfo.u32Width != d->output_width ||
        output.stCommFrameInfo.u32Height != d->output_height) {
        VDEC_ReleaseFrame(d->channel, output.ulBufferId);
        d->failed = true;
        return VT_IO_ERROR;
    }
    auto lease = std::unique_ptr<Lease>(new (std::nothrow) Lease{d});
    if (!lease) {
        VDEC_ReleaseFrame(d->channel, output.ulBufferId);
        return VT_IO_ERROR;
    }
    auto result = vt_mpp_frame_wrap(&output, input->frame_number, release_frame, lease.get(), out);
    if (result != VT_OK) {
        VDEC_ReleaseFrame(d->channel, output.ulBufferId);
        return result;
    }
    lease.release();
    return VT_OK;
}
extern "C" VT_API void vt_mpp_mjpeg_close(vt_mpp_mjpeg* self) { delete self; }
