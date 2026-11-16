#include "camera_adapter.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <uvc/uvc_api.h>
#include <vi/vi_api.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <video_transmission/legacy_api.hpp>

#include "example_config.hpp"

namespace video_transmission::example {
namespace {
void Check(int result, const char* operation) {
    if (result) throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}

void LogError(int result, const char* operation) noexcept {
    if (result) std::fprintf(stderr, "MPP capture: %s failed: %d\n", operation, result);
}

// The native sky example exclusively owns the process-wide VI module. Direct
// MPP VI users must not run alongside it; the VI API has no shared refcount.
enum class ViState { Available, Active, Unsafe };
struct ViModule {
    std::mutex mutex;
    ViState state = ViState::Available;
    std::shared_ptr<MppRuntime> retained_runtime;
    int retained_lock_fd = -1;
};
ViModule& ProcessViModule() {
    // Unsafe VI state may still use SYS/VB and the device lock. Keep the
    // module alive until OS process teardown rather than running destructors.
    static ViModule* const module = new ViModule;
    return *module;
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
    bool vi_owned = false;
    bool vi_setup_unsafe = false;
    std::atomic<bool> vi_release_failed{false};

    explicit CaptureDevice(const CaptureConfig& capture)
        : id(capture.device_id), channel(capture.channel_id), vi(capture.driver == "mpp_vi") {}

    // Caller holds module.mutex. The process owns these resources until exit;
    // dropping either one could disrupt resources in an uncertain state.
    void RetainUnsafe(ViModule& module) noexcept {
        module.state = ViState::Unsafe;
        module.retained_runtime = std::move(runtime);
        module.retained_lock_fd = std::exchange(lock_fd, -1);
    }

    void CheckViSetup(int result, const char* operation) {
        if (!result) return;
        auto& module = ProcessViModule();
        std::lock_guard lock(module.mutex);
        // MPP may have allocated device, sensor or channel resources even
        // when setup fails; partial setup has no guaranteed safe rollback.
        vi_setup_unsafe = true;
        RetainUnsafe(module);
        Check(result, operation);
    }

    void Initialize(const CaptureConfig& capture) {
        std::string key = vi ? "vi-" + std::to_string(id) : capture.device;
        for (char& ch : key)
            if (ch == '/') ch = '_';
        const auto path = "/tmp/hub-camera-" + key + ".lock";
        lock_fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock_fd < 0) Check(-errno, "open capture resource lock");
        if (flock(lock_fd, LOCK_EX | LOCK_NB)) Check(-errno, "flock capture resource lock");

        if (vi) {
            {
                auto& module = ProcessViModule();
                std::lock_guard lock(module.mutex);
                if (module.state != ViState::Available)
                    throw std::runtime_error("process VI module is already in use or unsafe");
                const int result = VI_Init();
                if (result) {
                    // A failed MPP init can leave a partially loaded module.
                    RetainUnsafe(module);
                    Check(result, "VI_Init");
                }
                module.state = ViState::Active;
                vi_owned = true;
            }
            ViDevAttrS device_attr{};
            device_attr.eWorkMode = VI_WORK_MODE_ONLINE;
            device_attr.u32Width = capture.width;
            device_attr.u32Height = capture.height;
            device_attr.u32MipiLaneNum = 2;
            CheckViSetup(VI_SetDevAttr(id, &device_attr), "VI_SetDevAttr");
            CheckViSetup(VI_EnableDev(id), "VI_EnableDev");
            enabled = true;
            ViChnAttrS channel_attr{};
            channel_attr.ePixelFormat = MPP_PIXEL_FORMAT_NV12;
            channel_attr.u32Width = capture.width;
            channel_attr.u32Height = capture.height;
            channel_attr.u32Depth = capture.depth;
            CheckViSetup(VI_SetChnAttr(id, channel, &channel_attr), "VI_SetChnAttr");
            CheckViSetup(VI_EnableChn(id, channel), "VI_EnableChn");
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
        if (vi_setup_unsafe) {
            std::fprintf(stderr, "MPP capture: VI cleanup skipped after device or channel setup failure\n");
            return;
        }
        if (vi) {
            if (vi_release_failed.load(std::memory_order_acquire)) {
                auto& module = ProcessViModule();
                std::lock_guard lock(module.mutex);
                // A failed frame release can leave a live VI buffer behind.
                std::fprintf(stderr, "MPP capture: VI cleanup skipped after frame release failure\n");
                RetainUnsafe(module);
                return;
            }
            bool safely_disabled = true;
            if (channel_enabled) {
                const int result = VI_DisableChn(id, channel);
                LogError(result, "VI_DisableChn");
                safely_disabled = result == 0;
            }
            if (enabled && safely_disabled) {
                const int result = VI_DisableDev(id);
                LogError(result, "VI_DisableDev");
                safely_disabled = result == 0;
            } else if (enabled) {
                std::fprintf(stderr, "MPP capture: VI_DisableDev skipped; channel is still enabled\n");
            }
            if (vi_owned) {
                auto& module = ProcessViModule();
                std::lock_guard lock(module.mutex);
                if (safely_disabled) {
                    const int result = VI_DeInit();
                    LogError(result, "VI_DeInit");
                    if (result) {
                        RetainUnsafe(module);
                    } else {
                        module.state = ViState::Available;
                    }
                } else {
                    // Do not unload a module that can still have live devices.
                    std::fprintf(stderr, "MPP capture: VI_DeInit skipped; VI may still be active\n");
                    RetainUnsafe(module);
                }
            }
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
    if (device->vi && device->vi_release_failed.load(std::memory_order_acquire)) return {};
    VideoFrameInfo info{};
    const int result = device->vi
        ? VI_GetChnFrame(device->id, device->channel, &info, static_cast<int>(timeout.count()))
        : UVC_GetFrame(device->id, device->channel, &info, static_cast<int>(timeout.count()));
    if (result) return {};
    // This MPP UVC producer stores V4L2's sequence counter in u32FrameFlag,
    // whose bit 0 means EOS to downstream MPP consumers. Live capture frames
    // are not EOS; the adapter keeps its own frame_number below.
    if (!device->vi) info.stVFrame.u32FrameFlag = 0;
    const auto release = [device](const VideoFrameInfo& value) {
        if (device->vi) {
            const int result = VI_ReleaseChnFrame(device->id, device->channel, &value);
            if (result) device->vi_release_failed.store(true, std::memory_order_release);
            LogError(result, "VI_ReleaseChnFrame");
        } else {
            LogError(UVC_ReleaseFrame(device->id, device->channel, &value), "UVC_ReleaseFrame");
        }
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

}  // namespace video_transmission::example
