#include <video_transmission/mpp_camera.h>
#include <uvc/uvc_api.h>
#include <cassert>
namespace { int creates, destroys, disables, releases; bool fail_channel, fail_get; }
extern "C" {
S32 UVC_CreateDev(UVC_DEV, const UvcDevAttr *) { ++creates; return 0; }
S32 UVC_EnableDev(UVC_DEV) { return 0; }
S32 UVC_SetChnAttr(UVC_DEV, UVC_CHN, const UvcChnAttr *) { return 0; }
S32 UVC_EnableChn(UVC_DEV, UVC_CHN) { return fail_channel ? -1 : 0; }
S32 UVC_DisableChn(UVC_DEV, UVC_CHN) { ++disables; return 0; }
S32 UVC_DisableDev(UVC_DEV) { return 0; }
S32 UVC_DestroyDev(UVC_DEV) { ++destroys; return 0; }
S32 UVC_GetFrame(UVC_DEV, UVC_CHN, VideoFrameInfo *f, S32) {
    if (fail_get) return -1;
    f->eFrameType = FRAME_TYPE_COMMON; f->stCommFrameInfo.u32Width = 640;
    f->stCommFrameInfo.u32Height = 480; f->stCommFrameInfo.ePixelFormat = MPP_PIXEL_FORMAT_NV12;
    f->stVFrame.u32FrameFlag = 1; /* UVC uses this field for sequence. */
    f->ulBufferId = 17; return 0;
}
S32 UVC_ReleaseFrame(UVC_DEV, UVC_CHN, const VideoFrameInfo *f) {
    assert(f->ulBufferId == 17); ++releases; return 0;
}
}
int main() {
    vt_mpp_camera_config cfg{sizeof(cfg), "/dev/video0", 0, 0, 640, 480, 30, 2, MPP_PIXEL_FORMAT_NV12};
    vt_mpp_camera *camera = nullptr;
    assert(vt_mpp_camera_open(&cfg, &camera) == VT_OK);
    vt_raw_frame frame{};
    fail_get = true;
    assert(vt_mpp_camera_get(camera, 1, &frame) == VT_IO_ERROR);
    fail_get = false;
    assert(vt_mpp_camera_get(camera, 1, &frame) == VT_OK);
    assert(vt_mpp_frame_info(&frame)->ulBufferId == 17);
    vt_mpp_camera_close(camera);
    assert(destroys == 0 && disables == 0);
    frame.release(frame.native_buffer);
    assert(releases == 1 && disables == 1 && destroys == 1);
    fail_channel = true;
    assert(vt_mpp_camera_open(&cfg, &camera) == VT_IO_ERROR);
    assert(camera == nullptr && creates == 2 && destroys == 2);
}
