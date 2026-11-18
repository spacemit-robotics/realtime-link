#include <video_transmission/mpp_v2d.h>
#include <v2d/v2d_api.h>
#include <limits>
extern "C" VT_API vt_status vt_mpp_crop_resize(
    const vt_raw_frame* input, const vt_raw_frame* output, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    const auto* src = vt_mpp_frame_info(input);
    const auto* dst = vt_mpp_frame_info(output);
    constexpr uint32_t limit = std::numeric_limits<U16>::max();
    if (!src || !dst || input->pixel_format != MPP_PIXEL_FORMAT_NV12 || output->pixel_format != MPP_PIXEL_FORMAT_NV12 ||
        !width || !height || !output->width || !output->height || x > input->width || y > input->height ||
        width > input->width - x || height > input->height - y || x > limit || y > limit || width > limit ||
        height > limit || output->width > limit || output->height > limit ||
        ((x | y | width | height | output->width | output->height) & 1))
        return VT_INVALID_ARGUMENT;
    V2DArea source{static_cast<U16>(x), static_cast<U16>(y), static_cast<U16>(width), static_cast<U16>(height)};
    V2DArea destination{0, 0, static_cast<U16>(output->width), static_cast<U16>(output->height)};
    VideoFrameInfo writable = *dst;
    V2DHandle job{};
    if (V2D_BeginJob(&job)) return VT_IO_ERROR;
    if (V2D_AddBitblitTask(job, src, &source, &writable, &destination, V2D_CSC_MODE_BUTT)) {
        V2D_CancelJob(job);
        return VT_IO_ERROR;
    }
    return V2D_EndJob(job) == 0 ? VT_OK : VT_IO_ERROR;
}
