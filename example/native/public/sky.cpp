#include "common.hpp"
#include <memory>
#include <sys/sys_api.h>
#include <sys/vb_api.h>
#include <uvc/uvc_api.h>
#include <venc/venc_api.h>
#include <vdec/vdec_api.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
namespace {
volatile std::sig_atomic_t quit = 0;
void sig(int) { quit = 1; }
struct Pool {
    UL id = 0;
    ~Pool() {
        if (id) VB_DestroyPool(id);
    }
};
void release_buffer(void* p, const VideoFrameInfo* info) {
    std::unique_ptr<std::shared_ptr<Pool>> held(static_cast<std::shared_ptr<Pool>*>(p));
    VB_ReleaseBuffer(info->ulBufferId);
}
std::shared_ptr<Pool> make_pool(uint32_t w, uint32_t h) {
    VideoFrameInfo info{};
    info.eFrameType = FRAME_TYPE_COMMON;
    info.stCommFrameInfo.u32Width = w;
    info.stCommFrameInfo.u32Height = h;
    info.stCommFrameInfo.u32Align = 16;
    info.stCommFrameInfo.ePixelFormat = MPP_PIXEL_FORMAT_NV12;
    const auto size = VB_GetPicBufferSize(&info);
    if (size <= 0) throw std::runtime_error("VB size failed");
    VbPoolCfg cfg{};
    cfg.u32BufSize = size;
    cfg.u32BufCnt = 8;
    cfg.eModId = MPP_ID_VENC;
    cfg.eRemapMode = VBUF_REMAP_MODE_NOCACHE;
    auto pool = std::make_shared<Pool>();
    pool->id = VB_CreatePool(&cfg);
    if (!pool->id || VB_SetFrameInfo(pool->id, &info)) throw std::runtime_error("VB pool failed");
    return pool;
}
vt_status crop(const std::shared_ptr<Pool>& pool, const vt_raw_frame& input, uint32_t x, uint32_t y, uint32_t w,
    uint32_t h, vt_raw_frame* out) {
    auto held = std::make_unique<std::shared_ptr<Pool>>(pool);
    UL id = VB_GetBuffer(pool->id, 100);
    if (!id) return VT_NOT_READY;
    VideoFrameInfo info{};
    if (VB_GetFrameInfo(id, &info)) {
        VB_ReleaseBuffer(id);
        return VT_IO_ERROR;
    }
    info.stVFrame.u64PTS = input.pts_us;
    auto rc = vt_mpp_frame_wrap(&info, input.frame_number, release_buffer, held.get(), out);
    if (rc != VT_OK) {
        VB_ReleaseBuffer(id);
        return rc;
    }
    held.release();
    rc = vt_mpp_crop_resize(&input, out, x, y, w, h);
    if (rc != VT_OK) {
        out->release(out->native_buffer);
        *out = {};
    }
    return rc;
}
}  // namespace
int main(int argc, char** argv) {
    if (argc != 12 && argc != 16) {
        std::fprintf(stderr,
            "usage: %s PLUGIN CONFIG CAMERA SECONDS CAP_WIDTH CAP_HEIGHT WIDTH HEIGHT FPS BITRATE GOP [ROI_X ROI_Y "
            "ROI_WIDTH ROI_HEIGHT]\n",
            argv[0]);
        return 2;
    }
    uint32_t seconds, cw, ch, w, h, fps, bitrate, gop, rx = 0, ry = 0, rw = 0, rh = 0;
    try {
        seconds = native_number(argv[4]);
        cw = native_number(argv[5]);
        ch = native_number(argv[6]);
        w = native_number(argv[7]);
        h = native_number(argv[8]);
        fps = native_number(argv[9]);
        bitrate = native_number(argv[10]);
        gop = native_number(argv[11]);
        if (argc == 16) {
            rx = native_offset(argv[12]);
            ry = native_offset(argv[13]);
            rw = native_number(argv[14]);
            rh = native_number(argv[15]);
        }
    } catch (...) {
        return 2;
    }
    if (SYS_Init() || VB_Init() || UVC_Init() || VDEC_Init() || VENC_Init()) {
        std::fprintf(stderr, "MPP init failed\n");
        return 1;
    }
    int result = 1;
    try {
        NativePlugin plugin(argv[1]);
        NativeEvents events;
        NativeSession session;
        std::unique_ptr<vt_mpp_camera, decltype(&vt_mpp_camera_close)> camera(nullptr, vt_mpp_camera_close);
        std::unique_ptr<vt_mpp_mjpeg, decltype(&vt_mpp_mjpeg_close)> jpeg(nullptr, vt_mpp_mjpeg_close);
        vt_mpp_camera* camera_ptr = nullptr;
        vt_mpp_mjpeg* jpeg_ptr = nullptr;
        vt_mpp_camera_config camera_cfg{sizeof(camera_cfg), argv[3], 0, 0, cw, ch, fps, 2, MPP_PIXEL_FORMAT_MJPEG};
        if (vt_mpp_camera_open(&camera_cfg, &camera_ptr) != VT_OK) throw std::runtime_error("camera open failed");
        camera.reset(camera_ptr);
        if (vt_mpp_mjpeg_open_scaled(0, cw, ch, argc == 16 ? cw : w, argc == 16 ? ch : h, &jpeg_ptr) != VT_OK)
            throw std::runtime_error("mjpeg open failed");
        jpeg.reset(jpeg_ptr);
        auto pool = argc == 16 ? make_pool(w, h) : std::shared_ptr<Pool>{};
        std::signal(SIGINT, sig);
        std::signal(SIGTERM, sig);
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        unsigned long submitted = 0, epoch = 0;
        NativeRequestRetry retry;
        while (!quit && std::chrono::steady_clock::now() < end) {
            if (!session.get() || events.reconnect.exchange(false)) {
                session.reset();
                auto rc = session.open(plugin, argv[2], VT_SKY, events, w, h, fps, bitrate, gop, nullptr);
                if (rc != VT_OK) {
                    std::fprintf(stderr, "session start status=%d\n", rc);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
                retry = {};
                std::printf("sky epoch=%lu\n", ++epoch);
                std::fflush(stdout);
            }
            if (events.keyframe.exchange(false)) retry.signal();
            const auto now = NativeRequestRetry::Clock::now();
            if (retry.ready(now)) retry.finish(session.request_local_keyframe(), now);
            vt_raw_frame input{}, output{};
            auto rc = vt_mpp_camera_get(camera.get(), 1000, &input);
            if (rc != VT_OK) {
                std::fprintf(stderr, "camera get status=%d\n", rc);
                continue;
            }
            rc = vt_mpp_mjpeg_decode(jpeg.get(), &input, &output);
            input.release(input.native_buffer);
            if (rc != VT_OK) {
                std::fprintf(stderr, "mjpeg status=%d\n", rc);
                jpeg.reset();
                jpeg_ptr = nullptr;
                if (vt_mpp_mjpeg_open_scaled(0, cw, ch, argc == 16 ? cw : w, argc == 16 ? ch : h, &jpeg_ptr) != VT_OK)
                    break;
                jpeg.reset(jpeg_ptr);
                continue;
            }
            if (pool) {
                vt_raw_frame cropped{};
                rc = crop(pool, output, rx, ry, rw, rh, &cropped);
                output.release(output.native_buffer);
                if (rc != VT_OK) {
                    std::fprintf(stderr, "crop status=%d\n", rc);
                    continue;
                }
                output = cropped;
            }
            rc = vt_session_submit_raw(session.get(), &output);
            output.release(output.native_buffer);
            if (rc == VT_OK)
                ++submitted;
            else
                std::fprintf(stderr, "submit status=%d\n", rc);
        }
        session.reset();
        jpeg.reset();
        camera.reset();
        pool.reset();
        std::printf("sky submitted=%lu epochs=%lu\n", submitted, epoch);
        result = submitted ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
    }
    VENC_Exit();
    VDEC_Exit();
    UVC_Exit();
    VB_Exit();
    SYS_Exit();
    return result;
}
