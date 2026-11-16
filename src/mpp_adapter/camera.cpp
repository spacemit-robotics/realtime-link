#include <video_transmission/mpp_camera.h>
#include <uvc/uvc_api.h>
#include <cstring>
#include <memory>
#include <new>

namespace {
struct Device {
    int dev, chn;
    bool created = false, enabled = false, channel = false;
    ~Device() {
        if (channel) UVC_DisableChn(dev, chn);
        if (enabled) UVC_DisableDev(dev);
        if (created) UVC_DestroyDev(dev);
    }
};
struct Held {
    std::shared_ptr<Device> device;
};
void return_frame(void* user, const VideoFrameInfo* info) {
    std::unique_ptr<Held> held(static_cast<Held*>(user));
    UVC_ReleaseFrame(held->device->dev, held->device->chn, info);
}
}  // namespace
struct vt_mpp_camera {
    std::shared_ptr<Device> device;
    uint64_t next = 0;
};
extern "C" VT_API vt_status vt_mpp_camera_open(const vt_mpp_camera_config* cfg, vt_mpp_camera** out) {
    if (!out) return VT_INVALID_ARGUMENT;
    *out = nullptr;
    if (!cfg || cfg->struct_size < sizeof(*cfg) || !cfg->device_path || !*cfg->device_path ||
        std::strlen(cfg->device_path) >= sizeof(UvcDevAttr{}.acDevNode) || cfg->device_id < 0 ||
        cfg->device_id >= UVC_MAX_DEV_NUM || cfg->channel_id < 0 || cfg->channel_id >= UVC_MAX_CHN_NUM || !cfg->width ||
        !cfg->height || !cfg->fps || !cfg->depth)
        return VT_INVALID_ARGUMENT;
    try {
        auto device = std::make_shared<Device>();
        device->dev = cfg->device_id;
        device->chn = cfg->channel_id;
        UvcDevAttr attr{};
        std::memcpy(attr.acDevNode, cfg->device_path, std::strlen(cfg->device_path) + 1);
        if (UVC_CreateDev(device->dev, &attr)) return VT_IO_ERROR;
        device->created = true;
        if (UVC_EnableDev(device->dev)) return VT_IO_ERROR;
        device->enabled = true;
        UvcChnAttr channel{};
        channel.u32Width = cfg->width;
        channel.u32Height = cfg->height;
        channel.u32Fps = cfg->fps;
        channel.u32Depth = cfg->depth;
        channel.ePixelFormat = static_cast<MppPixelFormat>(cfg->pixel_format);
        if (UVC_SetChnAttr(device->dev, device->chn, &channel) || UVC_EnableChn(device->dev, device->chn))
            return VT_IO_ERROR;
        device->channel = true;
        *out = new vt_mpp_camera{std::move(device), 0};
        return VT_OK;
    } catch (...) {
        return VT_IO_ERROR;
    }
}
extern "C" VT_API vt_status vt_mpp_camera_get(vt_mpp_camera* camera, int32_t timeout, vt_raw_frame* out) {
    if (!camera || !out || timeout < 0) return VT_INVALID_ARGUMENT;
    *out = {};
    VideoFrameInfo info{};
    // The MPP public UVC API does not expose a stable timeout error enum.
    // Preserve failures as errors instead of assuming every failure is a timeout.
    if (UVC_GetFrame(camera->device->dev, camera->device->chn, &info, timeout)) return VT_IO_ERROR;
    try {
        auto held = std::make_unique<Held>();
        held->device = camera->device;
        const auto status = vt_mpp_frame_wrap(&info, camera->next, return_frame, held.get(), out);
        if (status != VT_OK) {
            UVC_ReleaseFrame(camera->device->dev, camera->device->chn, &info);
            return status;
        }
        ++camera->next;
        held.release();
        return VT_OK;
    } catch (...) {
        UVC_ReleaseFrame(camera->device->dev, camera->device->chn, &info);
        return VT_IO_ERROR;
    }
}
extern "C" VT_API void vt_mpp_camera_close(vt_mpp_camera* camera) { delete camera; }
