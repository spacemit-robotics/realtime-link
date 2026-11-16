#include <video_transmission/mpp_v2d.h>
#include <v2d/v2d_api.h>
#include <cassert>
namespace {
int begun, ended, cancelled;
bool fail_task;
V2DArea observed{};
}  // namespace
extern "C" {
S32 V2D_BeginJob(V2DHandle* h) {
    ++begun;
    *h = 1;
    return 0;
}
S32 V2D_AddBitblitTask(V2DHandle, const VideoFrameInfo*, V2DArea* src, VideoFrameInfo*, V2DArea*, V2DCscMode) {
    observed = *src;
    return fail_task ? -1 : 0;
}
S32 V2D_EndJob(V2DHandle) {
    ++ended;
    return 0;
}
S32 V2D_CancelJob(V2DHandle) {
    ++cancelled;
    return 0;
}
}
void done(void*, const VideoFrameInfo*) {}
int main() {
    VideoFrameInfo src{};
    src.eFrameType = FRAME_TYPE_COMMON;
    src.stCommFrameInfo.u32Width = 1280;
    src.stCommFrameInfo.u32Height = 720;
    src.stCommFrameInfo.ePixelFormat = MPP_PIXEL_FORMAT_NV12;
    VideoFrameInfo dst = src;
    dst.stCommFrameInfo.u32Width = 640;
    dst.stCommFrameInfo.u32Height = 360;
    vt_raw_frame a{}, b{};
    assert(vt_mpp_frame_wrap(&src, 0, done, nullptr, &a) == VT_OK);
    assert(vt_mpp_frame_wrap(&dst, 0, done, nullptr, &b) == VT_OK);
    assert(vt_mpp_crop_resize(&a, &b, 100, 20, 800, 400) == VT_OK);
    assert(begun == 1 && ended == 1 && cancelled == 0);
    assert(vt_mpp_crop_resize(&a, &b, 1001, 0, 500, 400) == VT_INVALID_ARGUMENT && begun == 1);
    assert(vt_mpp_crop_resize(&a, &b, 1, 0, 800, 400) == VT_INVALID_ARGUMENT && begun == 1);
    fail_task = true;
    assert(vt_mpp_crop_resize(&a, &b, 0, 0, 800, 400) == VT_IO_ERROR);
    assert(cancelled == 1 && ended == 1);
    a.release(a.native_buffer);
    b.release(b.native_buffer);
}
