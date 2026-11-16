#include <video_transmission/mpp_frame.h>
#include <atomic>
#include <new>

namespace {
struct Lease {
    std::atomic<unsigned> refs{1};
    VideoFrameInfo info;
    vt_mpp_release_fn release;
    void* user;
};
void retain(void* p) { static_cast<Lease*>(p)->refs.fetch_add(1, std::memory_order_relaxed); }
void release(void* p) {
    auto* lease = static_cast<Lease*>(p);
    if (lease->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        lease->release(lease->user, &lease->info);
        delete lease;
    }
}
}  // namespace
extern "C" VT_API vt_status vt_mpp_frame_wrap(
    const VideoFrameInfo* info, uint64_t number, vt_mpp_release_fn fn, void* user, vt_raw_frame* out) {
    if (!out) return VT_INVALID_ARGUMENT;
    *out = {};
    if (!info || !fn ||
        (info->eFrameType != FRAME_TYPE_COMMON && info->eFrameType != FRAME_TYPE_VI &&
            info->eFrameType != FRAME_TYPE_CPP && info->eFrameType != FRAME_TYPE_VO &&
            info->eFrameType != FRAME_TYPE_VENC && info->eFrameType != FRAME_TYPE_VDEC) ||
        (info->eFrameType == FRAME_TYPE_VDEC && (info->stVFrame.u32FrameFlag & MPP_FRAME_FLAG_EOS)) ||
        !info->stCommFrameInfo.u32Width || !info->stCommFrameInfo.u32Height)
        return VT_INVALID_ARGUMENT;
    auto* lease = new (std::nothrow) Lease{{1}, *info, fn, user};
    if (!lease) return VT_IO_ERROR;
    *out = {sizeof(vt_raw_frame), lease, retain, release, static_cast<uint32_t>(info->stCommFrameInfo.ePixelFormat),
        info->stCommFrameInfo.u32Width, info->stCommFrameInfo.u32Height, number, info->stVFrame.u64PTS};
    return VT_OK;
}
extern "C" VT_API const VideoFrameInfo* vt_mpp_frame_info(const vt_raw_frame* frame) {
    if (!frame || frame->struct_size < sizeof(vt_raw_frame) || !frame->native_buffer || frame->retain != retain ||
        frame->release != release)
        return nullptr;
    return &static_cast<Lease*>(frame->native_buffer)->info;
}
