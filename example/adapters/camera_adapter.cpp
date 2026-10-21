#include "camera_adapter.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <uvc/uvc_api.h>
#include <vi/vi_api.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <video_transmission/control.hpp>
#include <video_transmission/latency.hpp>
#include <video_transmission/mjpeg_decoder.hpp>
#include <video_transmission/mpp_frame.hpp>

#include "example_config.hpp"

namespace video_transmission::example {
namespace {
void Check(int result, const char* operation) {
    if (result) throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}

void LogError(int result, const char* operation) noexcept {
    if (result) std::fprintf(stderr, "MPP capture: %s failed: %d\n", operation, result);
}

struct CaptureDevice {
    std::shared_ptr<MppRuntime> runtime = std::make_shared<MppRuntime>();
    int lock_fd = -1;
    int id;
    int channel;
    bool vi;
    bool created = false;
    bool enabled = false;
    bool channel_enabled = false;

    explicit CaptureDevice(const CaptureConfig& capture)
        : id(capture.device_id), channel(capture.channel_id), vi(capture.driver == "mpp_vi") {}

    void Initialize(const CaptureConfig& capture) {
        std::string key = vi ? "vi-" + std::to_string(id) : capture.device;
        for (char& ch : key)
            if (ch == '/') ch = '_';
        const auto path = "/tmp/hub-camera-" + key + ".lock";
        lock_fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock_fd < 0) Check(-errno, "open capture resource lock");
        if (flock(lock_fd, LOCK_EX | LOCK_NB)) Check(-errno, "flock capture resource lock");

        if (vi) {
            Check(VI_Init(), "VI_Init");
            ViDevAttrS device_attr{};
            device_attr.eWorkMode = VI_WORK_MODE_ONLINE;
            device_attr.u32Width = capture.width;
            device_attr.u32Height = capture.height;
            device_attr.u32MipiLaneNum = 2;
            Check(VI_SetDevAttr(id, &device_attr), "VI_SetDevAttr");
            Check(VI_EnableDev(id), "VI_EnableDev");
            enabled = true;
            ViChnAttrS channel_attr{};
            channel_attr.ePixelFormat = MPP_PIXEL_FORMAT_NV12;
            channel_attr.u32Width = capture.width;
            channel_attr.u32Height = capture.height;
            channel_attr.u32Depth = capture.depth;
            Check(VI_SetChnAttr(id, channel, &channel_attr), "VI_SetChnAttr");
            Check(VI_EnableChn(id, channel), "VI_EnableChn");
            channel_enabled = true;
            return;
        }
        Check(UVC_Init(), "UVC_Init");
        UvcDevAttr device_attr{};
        std::memcpy(device_attr.acDevNode, capture.device.c_str(), capture.device.size() + 1);
        Check(UVC_CreateDev(id, &device_attr), "UVC_CreateDev");
        created = true;
        Check(UVC_EnableDev(id), "UVC_EnableDev");
        enabled = true;
        UvcChnAttr channel_attr{};
        channel_attr.u32Width = capture.width;
        channel_attr.u32Height = capture.height;
        channel_attr.u32Fps = capture.fps;
        channel_attr.u32Depth = capture.depth;
        channel_attr.ePixelFormat =
            capture.format == "mjpeg" ? MPP_PIXEL_FORMAT_MJPEG : MPP_PIXEL_FORMAT_NV12;
        Check(UVC_SetChnAttr(id, channel, &channel_attr), "UVC_SetChnAttr");
        Check(UVC_EnableChn(id, channel), "UVC_EnableChn");
        channel_enabled = true;
    }

    ~CaptureDevice() noexcept {
        if (vi) {
            if (channel_enabled) LogError(VI_DisableChn(id, channel), "VI_DisableChn");
            if (enabled) LogError(VI_DisableDev(id), "VI_DisableDev");
        } else {
            if (channel_enabled) LogError(UVC_DisableChn(id, channel), "UVC_DisableChn");
            if (enabled) LogError(UVC_DisableDev(id), "UVC_DisableDev");
            if (created) LogError(UVC_DestroyDev(id), "UVC_DestroyDev");
        }
        if (lock_fd >= 0) close(lock_fd);
        // UVC_Exit affects other devices owned by this process.
    }
};

std::shared_ptr<CaptureDevice> OpenDevice(const CaptureConfig& capture) {
    auto device = std::make_shared<CaptureDevice>(capture);
    device->Initialize(capture);
    return device;
}
}  // namespace

struct CameraAdapter::Impl {
    std::shared_ptr<CaptureDevice> device;
    std::unique_ptr<video_transmission::MjpegDecoder> decoder;
    CaptureConfig capture;
    uint64_t sequence = 0;
    std::mutex mutex;
    bool stopped = false;
};

CameraAdapter::CameraAdapter(const ExampleConfig& config) : impl_(std::make_unique<Impl>()) {
    ValidateExampleConfig(config);
    if (config.session.role != Role::Sky || config.session.input_mode != InputMode::Raw)
        throw std::invalid_argument("camera adapter requires sky/raw");
    if (config.capture.driver != "mpp_uvc" && config.capture.driver != "mpp_vi")
        throw std::invalid_argument("native sky example only supports mpp_uvc or mpp_vi");
    impl_->capture = config.capture;
    impl_->device = OpenDevice(config.capture);
    if (config.capture.format == "mjpeg")
        impl_->decoder = std::make_unique<video_transmission::MjpegDecoder>(
            config.session.pipeline.decoder_id, config.capture.width, config.capture.height);
}

CameraAdapter::~CameraAdapter() = default;

MppFrame CameraAdapter::Get(std::chrono::milliseconds timeout) {
    std::lock_guard lock(impl_->mutex);
    auto device = impl_->device;
    if (impl_->stopped || !device) return {};
    VideoFrameInfo info{};
    const int result = device->vi
        ? VI_GetChnFrame(device->id, device->channel, &info, static_cast<int>(timeout.count()))
        : UVC_GetFrame(device->id, device->channel, &info, static_cast<int>(timeout.count()));
    if (result) return {};
    const auto release = [device](const VideoFrameInfo& value) {
        if (device->vi)
            LogError(VI_ReleaseChnFrame(device->id, device->channel, &value), "VI_ReleaseChnFrame");
        else
            LogError(UVC_ReleaseFrame(device->id, device->channel, &value), "UVC_ReleaseFrame");
    };
    std::function<void(const VideoFrameInfo&)> release_callback;
    try {
        release_callback = release;
    } catch (...) {
        release(info);
        throw;
    }
    auto frame = AdoptMppFrame(
        info, std::move(release_callback), device, impl_->capture.channel, impl_->sequence++);
    latency::Mark("capture_ready", frame.info.stVFrame.u64PTS, frame.info.stVFrame.u32TotalSize);
    if (impl_->capture.format == "mjpeg") {
        if (frame.info.stCommFrameInfo.ePixelFormat != MPP_PIXEL_FORMAT_MJPEG)
            throw std::runtime_error("MPP capture did not produce configured MJPEG");
        return impl_->decoder->Decode(frame);
    }
    if (frame.info.stCommFrameInfo.ePixelFormat != MPP_PIXEL_FORMAT_NV12)
        throw std::runtime_error("MPP capture did not produce configured NV12");
    ValidateMppFrame(frame);
    return frame;
}

void CameraAdapter::Stop() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopped) return;
    impl_->stopped = true;
    // Outstanding frame leases keep the device enabled until the capture frame is released.
    impl_->device.reset();
}

ControlMessage CameraAdapter::HandleControl(const ControlMessage& message) {
    ControlMessage reply{message.tag + 1, std::vector<uint8_t>(136, 0)};
    // 现有协议的返回码为 payload[4..7] 小端有符号整数，-1 表示不支持。
    for (size_t index = 4; index < 8; ++index) reply.payload[index] = 0xff;
    return reply;
}
}  // namespace video_transmission::example
