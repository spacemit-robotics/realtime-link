#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/sys_api.h>
#include <sys/vb_api.h>
#include <unistd.h>
#include <vdec/vdec_api.h>

#include <cerrno>
#include <cstdio>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <video_transmission/mjpeg_decoder.hpp>
#include <video_transmission/mpp_frame.hpp>

namespace video_transmission {
namespace {
void Release(const std::function<void(const VideoFrameInfo&)>& release,
    const VideoFrameInfo& info) noexcept {
    try {
        release(info);
    } catch (...) {
        std::fprintf(stderr, "MPP frame release callback threw\n");
    }
}
struct FrameLease {
    VideoFrameInfo info;
    std::function<void(const VideoFrameInfo&)> release;
    std::shared_ptr<void> context;
    ~FrameLease() { Release(release, info); }
};
}  // namespace

MppRuntime::MppRuntime() {
    // 与所有适配层共享 MPP 动态库已有的进程计数，不设独立计数。
    int rc = SYS_Init();
    if (rc) throw std::runtime_error("SYS_Init failed: " + std::to_string(rc));
    rc = VB_Init();
    if (rc) {
        SYS_Exit();
        throw std::runtime_error("VB_Init failed: " + std::to_string(rc));
    }
}

MppRuntime::~MppRuntime() {
    if (int rc = VB_Exit()) std::fprintf(stderr, "VB_Exit failed: %d\n", rc);
    if (int rc = SYS_Exit()) std::fprintf(stderr, "SYS_Exit failed: %d\n", rc);
}

MppFrame AdoptMppFrame(VideoFrameInfo info, std::function<void(const VideoFrameInfo&)> release,
    std::shared_ptr<void> context, uint32_t channel, uint64_t frame_number, uint64_t generation) {
    if (!release) throw std::invalid_argument("MPP frame release callback is required");
    // 分配控制块前不移动 release/context，失败时仍能归还已取得的帧。
    std::shared_ptr<FrameLease> held;
    try {
        held = std::make_shared<FrameLease>();
    } catch (...) {
        Release(release, info);
        throw;
    }
    held->info = info;
    held->release = std::move(release);
    held->context = std::move(context);
    return {info, std::move(held), channel, frame_number, generation};
}

void ValidateMppFrame(const MppFrame& frame) {
    const auto& value = frame.info;
    const auto& common = value.stCommFrameInfo;
    const auto& buffer = value.stVFrame;
    const bool bgra = common.ePixelFormat == MPP_PIXEL_FORMAT_BGRA;
    const bool nv12 = common.ePixelFormat == MPP_PIXEL_FORMAT_NV12;
    if (!frame.owner || !value.ulBufferId || !common.u32Width || !common.u32Height ||
        common.u32Width > 16384 || common.u32Height > 16384 || common.u32Width % 2 ||
        common.u32Height % 2 || common.eCompressMode != COMPRESS_MODE_NONE || (!nv12 && !bgra) ||
        buffer.u32PlaneNum != (bgra ? 1U : 2U) || !buffer.u32TotalSize ||
        buffer.u32FrameFlag & MPP_FRAME_FLAG_EOS)
        throw std::invalid_argument("expected a leased, linear NV12 or BGRA MPP frame");
    VideoFrameInfo actual{};
    int fd = -1;
    if (VB_GetFrameInfo(value.ulBufferId, &actual) || VB_GetDmaBufFd(value.ulBufferId, &fd) ||
        fd < 0 || actual.stCommFrameInfo.u32Width != common.u32Width ||
        actual.stCommFrameInfo.u32Height != common.u32Height ||
        actual.stCommFrameInfo.ePixelFormat != common.ePixelFormat ||
        actual.stCommFrameInfo.eCompressMode != common.eCompressMode ||
        actual.stVFrame.u32PlaneNum != buffer.u32PlaneNum ||
        actual.stVFrame.u32TotalSize != buffer.u32TotalSize)
        throw std::invalid_argument("frame does not describe a local MPP VB buffer");
    uint64_t total = 0;
    for (uint32_t plane = 0; plane < buffer.u32PlaneNum; ++plane) {
        const uint64_t required =
            uint64_t(buffer.u32PlaneStride[plane]) * (common.u32Height / (plane + 1));
        if (buffer.u32PlaneStride[plane] < common.u32Width * (bgra ? 4U : 1U) ||
            required > buffer.u32PlaneSizeValid[plane] ||
            buffer.u32PlaneSizeValid[plane] > buffer.u32PlaneSize[plane] ||
            buffer.u32PlaneSize[plane] != actual.stVFrame.u32PlaneSize[plane] ||
            buffer.u32PlaneStride[plane] != actual.stVFrame.u32PlaneStride[plane] ||
            !buffer.ulPlaneVirAddr[plane] ||
            buffer.ulPlaneVirAddr[plane] != actual.stVFrame.ulPlaneVirAddr[plane] ||
            buffer.u32Fd[plane] > static_cast<UL>(std::numeric_limits<int>::max()) ||
            buffer.u32Fd[plane] != static_cast<UL>(fd) ||
            buffer.u64PlanePhyAddr[plane] != actual.stVFrame.u64PlanePhyAddr[plane])
            throw std::invalid_argument("invalid raw plane capacity, stride or mapping");
        total += buffer.u32PlaneSize[plane];
    }
    if (total > buffer.u32TotalSize) throw std::invalid_argument("planes exceed buffer capacity");
}
namespace {
void CheckMjpeg(int result, const char* operation) {
    if (result) throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
void LogMjpeg(int result, const char* operation) noexcept {
    if (result) std::fprintf(stderr, "MjpegDecoder: %s: %d\n", operation, result);
}
class DecoderLock {
public:
    explicit DecoderLock(int channel) {
        const auto path = "/tmp/hub-video_transmission-vdec-" + std::to_string(channel) + ".lock";
        fd_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd_ < 0) CheckMjpeg(-errno, "open VDEC lock");
        if (flock(fd_, LOCK_EX | LOCK_NB)) {
            int error = errno;
            close(fd_);
            fd_ = -1;
            CheckMjpeg(-error, "flock VDEC lock");
        }
    }
    ~DecoderLock() {
        if (fd_ >= 0) close(fd_);
    }
    DecoderLock(const DecoderLock&) = delete;
    DecoderLock& operator=(const DecoderLock&) = delete;

private:
    int fd_ = -1;
};
class DmaRead {
public:
    explicit DmaRead(int fd) : fd_(fd) {
        CheckMjpeg(Sync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ), "DMA read begin");
        active_ = true;
    }
    ~DmaRead() {
        if (active_) LogMjpeg(Sync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ), "DMA read end");
    }
    void Finish() {
        CheckMjpeg(Sync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ), "DMA read end");
        active_ = false;
    }

private:
    int Sync(uint64_t flags) noexcept {
        dma_buf_sync sync{flags};
        int result;
        do {
            result = ioctl(fd_, DMA_BUF_IOCTL_SYNC, &sync);
        } while (result < 0 && errno == EINTR);
        return result == 0 ? 0 : -errno;
    }
    int fd_;
    bool active_ = false;
};
}  // namespace

struct MjpegDecoder::Impl {
    MppRuntime runtime;
    DecoderLock resource;
    int id;
    uint32_t width, height;
    bool created = false, enabled = false, failed = false;
    std::mutex mutex;

    struct Lease {
        explicit Lease(std::shared_ptr<Impl> value) : decoder(std::move(value)) {}
        ~Lease() {
            if (acquired)
                LogMjpeg(VDEC_ReleaseFrame(decoder->id, info.ulBufferId), "VDEC_ReleaseFrame");
        }
        std::shared_ptr<Impl> decoder;
        VideoFrameInfo info{};
        bool acquired = false;
    };

    Impl(int channel, uint32_t w, uint32_t h)
        : resource(channel), id(channel), width(w), height(h) {}
    void Initialize() {
        int result = VDEC_Init();
        if (result != ERR_VDEC_ALREADY_INIT) CheckMjpeg(result, "VDEC_Init");
        VdecChnAttr attr{};
        attr.eCodecType = MPP_STREAM_CODEC_MJPEG;
        attr.eOutputPixelFormat = MPP_PIXEL_FORMAT_NV12;
        attr.u32Width = width;
        attr.u32Height = height;
        attr.u32Align = 16;
        attr.u32BufCnt = 8;
        CheckMjpeg(VDEC_CreateChn(id, &attr), "VDEC_CreateChn");
        created = true;
        CheckMjpeg(VDEC_EnableChn(id), "VDEC_EnableChn");
        enabled = true;
    }
    ~Impl() {
        if (enabled) LogMjpeg(VDEC_DisableChn(id), "VDEC_DisableChn");
        if (created) LogMjpeg(VDEC_DestroyChn(id), "VDEC_DestroyChn");
        // 不关闭模块级 VDEC，其他通道可能仍在使用。
    }
};

MjpegDecoder::MjpegDecoder(int decoder_id, uint32_t width, uint32_t height) {
    if (decoder_id < 0 || decoder_id >= VDEC_MAX_CHN || !width || !height || width > 16384 ||
        height > 16384 || width % 2 || height % 2)
        throw std::invalid_argument("invalid MJPEG decoder channel/dimensions");
    impl_ = std::make_shared<Impl>(decoder_id, width, height);
    impl_->Initialize();
}
MjpegDecoder::~MjpegDecoder() = default;

MppFrame MjpegDecoder::Decode(const MppFrame& input) {
    auto decoder = impl_;
    std::lock_guard lock(decoder->mutex);
    if (decoder->failed) throw std::logic_error("MJPEG decoder failed; recreate helper");
    const MppFrame held = input;
    const auto& common = held.info.stCommFrameInfo;
    const auto& buffer = held.info.stVFrame;
    VideoFrameInfo actual{};
    int fd = -1;
    if (!held.owner || !held.info.ulBufferId || held.channel > 1 ||
        common.ePixelFormat != MPP_PIXEL_FORMAT_MJPEG ||
        common.eCompressMode != COMPRESS_MODE_NONE || common.u32Width != decoder->width ||
        common.u32Height != decoder->height || buffer.u32PlaneNum != 1 || !buffer.u32TotalSize ||
        !buffer.u32PlaneSizeValid[0] || buffer.u32PlaneSizeValid[0] > buffer.u32PlaneSize[0] ||
        buffer.u32PlaneSize[0] > buffer.u32TotalSize || !buffer.ulPlaneVirAddr[0] ||
        (buffer.u32FrameFlag & MPP_FRAME_FLAG_EOS) ||
        VB_GetFrameInfo(held.info.ulBufferId, &actual) ||
        VB_GetDmaBufFd(held.info.ulBufferId, &fd) || fd < 0 ||
        buffer.u32Fd[0] != static_cast<UL>(fd) ||
        actual.stCommFrameInfo.ePixelFormat != common.ePixelFormat ||
        actual.stCommFrameInfo.eCompressMode != common.eCompressMode ||
        actual.stCommFrameInfo.u32Width != common.u32Width ||
        actual.stCommFrameInfo.u32Height != common.u32Height || actual.stVFrame.u32PlaneNum != 1 ||
        actual.stVFrame.u32TotalSize != buffer.u32TotalSize ||
        actual.stVFrame.u32PlaneSize[0] != buffer.u32PlaneSize[0] ||
        actual.stVFrame.ulPlaneVirAddr[0] != buffer.ulPlaneVirAddr[0])
        throw std::invalid_argument("expected a leased local VB with one mapped MJPEG plane");

    auto lease = std::make_shared<Impl::Lease>(decoder);
    StreamBufferInfo stream{};
    stream.pu8Addr = reinterpret_cast<const uint8_t*>(buffer.ulPlaneVirAddr[0]);
    stream.u32Size = buffer.u32PlaneSizeValid[0];
    stream.u64PTS = buffer.u64PTS;
    stream.eCodecType = MPP_STREAM_CODEC_MJPEG;
    try {
        {
            DmaRead read(fd);
            // Linlon copyInputPayload 在 SendStream 返回前复制至内部 MMAP 缓冲。
            CheckMjpeg(VDEC_SendStream(decoder->id, &stream, 100), "VDEC_SendStream");
            read.Finish();
        }
        CheckMjpeg(VDEC_GetFrame(decoder->id, &lease->info, 1000), "VDEC_GetFrame");
        lease->acquired = true;
        if (lease->info.stVFrame.u64PTS != buffer.u64PTS ||
            lease->info.stCommFrameInfo.u32Width != decoder->width ||
            lease->info.stCommFrameInfo.u32Height != decoder->height)
            throw std::runtime_error("MJPEG output geometry/PTS does not match input");
        MppFrame output{lease->info, lease, held.channel, held.frame_number, held.generation};
        ValidateMppFrame(output);
        return output;
    } catch (...) {
        // 迟到输出不能与下一输入绑定；超时或送入后失败必须重建。
        decoder->failed = true;
        throw;
    }
}
}  // namespace video_transmission
