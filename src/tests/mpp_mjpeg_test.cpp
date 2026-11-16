#include <video_transmission/mpp_mjpeg.h>
#include <vdec/vdec_api.h>
#include <cassert>
namespace {
int released, destroyed, syncs;
bool fail_sync;
U64 pts;
VdecChnAttr attr;
}  // namespace
extern "C" {
int ioctl(int, unsigned long, ...) {
    ++syncs;
    return fail_sync ? -1 : 0;
}
S32 VDEC_CreateChn(S32, const VdecChnAttr* a) {
    attr = *a;
    return 0;
}
S32 VDEC_EnableChn(S32) { return 0; }
S32 VDEC_DisableChn(S32) { return 0; }
S32 VDEC_DestroyChn(S32) {
    ++destroyed;
    return 0;
}
S32 VDEC_SendStream(S32, const StreamBufferInfo* s, U32) {
    pts = s->u64PTS;
    return 0;
}
S32 VDEC_GetFrame(S32, VideoFrameInfo* f, U32) {
    f->eFrameType = FRAME_TYPE_VDEC;
    f->stVdecFrameInfo.stCommFrameInfo.u32Width = attr.stScale.u32Width;
    f->stVdecFrameInfo.stCommFrameInfo.u32Height = attr.stScale.u32Height;
    f->stVdecFrameInfo.stCommFrameInfo.ePixelFormat = MPP_PIXEL_FORMAT_NV12;
    f->stVFrame.u64PTS = pts;
    f->ulBufferId = 42;
    return 0;
}
S32 VDEC_ReleaseFrame(S32, UL id) {
    assert(id == 42);
    ++released;
    return 0;
}
}
void release_input(void*, const VideoFrameInfo*) {}
int main() {
    vt_mpp_mjpeg* d = nullptr;
    vt_raw_frame in{}, out{};
    assert(vt_mpp_mjpeg_open_scaled(0, 1280, 720, 640, 360, &d) == VT_OK);
    assert(attr.stScale.bScaleEnable && attr.stScale.u32Width == 640 && attr.stScale.u32Height == 360);
    unsigned char bytes[8]{};
    VideoFrameInfo info{};
    info.eFrameType = FRAME_TYPE_COMMON;
    info.stCommFrameInfo.u32Width = 1280;
    info.stCommFrameInfo.u32Height = 720;
    info.stCommFrameInfo.ePixelFormat = MPP_PIXEL_FORMAT_MJPEG;
    info.stVFrame.u32PlaneNum = 1;
    info.stVFrame.ulPlaneVirAddr[0] = reinterpret_cast<UL>(bytes);
    info.stVFrame.u32PlaneSizeValid[0] = sizeof(bytes);
    info.stVFrame.u32PlaneSize[0] = sizeof(bytes);
    info.stVFrame.u32Fd[0] = 3;
    info.stVFrame.u64PTS = 99;
    assert(vt_mpp_frame_wrap(&info, 7, release_input, nullptr, &in) == VT_OK);
    assert(vt_mpp_mjpeg_decode(d, &in, &out) == VT_OK);
    assert(out.width == 640 && out.height == 360 && out.pts_us == 99 && syncs == 2);
    vt_mpp_mjpeg_close(d);
    assert(!destroyed);
    out.release(out.native_buffer);
    assert(released == 1 && destroyed == 1);
    assert(vt_mpp_mjpeg_open_scaled(0, 1280, 720, 640, 360, &d) == VT_OK);
    fail_sync = true;
    assert(vt_mpp_mjpeg_decode(d, &in, &out) == VT_IO_ERROR);
    assert(vt_mpp_mjpeg_decode(d, &in, &out) == VT_NOT_READY);
    vt_mpp_mjpeg_close(d);
    in.release(in.native_buffer);
}
