// 显式 MPP 帧输入、编解码及通信会话生命周期；采集由外部适配器负责。
#include <fcntl.h>
#include <sys/file.h>
#include <sys/vb_api.h>
#include <unistd.h>
#include <v2d/v2d_api.h>
#include <vdec/vdec_api.h>
#include <venc/venc_api.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <video_transmission/fpv_session.hpp>
#include <video_transmission/latency.hpp>

#include "transport/frame_timing.hpp"
namespace video_transmission {
namespace {
thread_local const void* callback_session = nullptr;
struct SessionThread {
    const void* previous;
    explicit SessionThread(const void* session) : previous(callback_session) {
        callback_session = session;
    }
    ~SessionThread() { callback_session = previous; }
};
void Check(int rc, const char* op) {
    if (rc) throw std::runtime_error(std::string(op) + ": " + std::to_string(rc));
}
void ValidateRawFrame(const MppFrame& frame, const PipelineConfig& config) {
    ValidateMppFrame(frame);
    const auto& info = frame.info.stCommFrameInfo;
    const uint32_t width = config.roi_width ? config.roi_width : info.u32Width;
    const uint32_t height = config.roi_height ? config.roi_height : info.u32Height;
    const auto limit = std::numeric_limits<U16>::max();
    // 在窄化到 V2DArea 前检查，使用减法避免 ROI 端点溢出。
    if ((info.ePixelFormat != MPP_PIXEL_FORMAT_NV12 &&
            info.ePixelFormat != MPP_PIXEL_FORMAT_BGRA) ||
        info.eCompressMode != COMPRESS_MODE_NONE || config.roi_x > info.u32Width ||
        config.roi_y > info.u32Height || width > info.u32Width - config.roi_x ||
        height > info.u32Height - config.roi_y || !width || !height ||
        (config.roi_x | config.roi_y | width | height) % 2 || config.roi_x > limit ||
        config.roi_y > limit || width > limit || height > limit || !config.width ||
        !config.height || config.width > limit || config.height > limit)
        throw std::invalid_argument("invalid linear raw frame or ROI outside actual source");
    // Direct packed input uses the codec's negotiated, tightly packed stride.
    // Do not silently reinterpret padding, crop, or resize a BGRA frame.
    if (info.ePixelFormat == MPP_PIXEL_FORMAT_BGRA &&
        (config.roi_x || config.roi_y || width != info.u32Width || height != info.u32Height ||
            info.u32Width != config.width || info.u32Height != config.height ||
            info.u32Width % 16 || frame.info.stVFrame.u32PlaneStride[0] != info.u32Width * 4U))
        throw std::invalid_argument(
            "direct BGRA requires full-size input, 16-aligned width and width*4 stride");
}
// Deployment IDs are guarded across processes; never unlink a live lock file.
struct Resource {
    int fd = -1;
    Resource(const std::string& kind, int id) {
        auto path = "/tmp/hub-fpv-" + kind + "-" + std::to_string(id) + ".lock";
        fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB)) {
            if (fd >= 0) close(fd);
            fd = -1;
            throw std::runtime_error("MPP resource busy: " + kind + " " + std::to_string(id));
        }
    }
    ~Resource() {
        if (fd >= 0) close(fd);
    }
};
struct Decoder : std::enable_shared_from_this<Decoder> {
    std::shared_ptr<MppRuntime> runtime = std::make_shared<MppRuntime>();
    Resource resource;
    int id;
    MppStreamCodecType codec_type;
    bool created = false, enabled = false;
    Decoder(int channel, MppStreamCodecType codec, uint32_t w, uint32_t h)
        : resource("vdec", channel), id(channel), codec_type(codec) {
        int rc = VDEC_Init();
        if (rc != 0 && rc != ERR_VDEC_ALREADY_INIT) Check(rc, "VDEC_Init");
        VdecChnAttr a{};
        a.eCodecType = codec;
        a.eOutputPixelFormat = MPP_PIXEL_FORMAT_NV12;
        a.u32Width = w;
        a.u32Height = h;
        a.u32Align = 16;
        a.u32BufCnt = 8;
        Check(VDEC_CreateChn(id, &a), "VDEC_CreateChn");
        created = true;
        rc = VDEC_EnableChn(id);
        if (rc) {
            VDEC_DestroyChn(id);
            created = false;
            Check(rc, "VDEC_EnableChn");
        }
        enabled = true;
    }
    ~Decoder() {
        if (enabled) VDEC_DisableChn(id);
        if (created) VDEC_DestroyChn(id);
    }
    void Send(const uint8_t* ptr, size_t bytes, uint64_t pts, MppStreamCodecType codec) {
        StreamBufferInfo s{};
        s.pu8Addr = ptr;
        s.u32Size = bytes;
        s.u64PTS = pts;
        s.eCodecType = codec;
        latency::Mark("decode_submit", pts, bytes);
        Check(VDEC_SendStream(id, &s, 100), "VDEC_SendStream");
    }
    MppFrame Get() {
        auto self = shared_from_this();
        std::function<void(const VideoFrameInfo&)> release = [self](const auto& f) {
            VDEC_ReleaseFrame(self->id, f.ulBufferId);
        };
        VideoFrameInfo v{};
        if (VDEC_GetFrame(id, &v, 20)) return {};
        latency::Mark("decode_done", v.stVFrame.u64PTS);
        return AdoptMppFrame(v, std::move(release), self);
    }
};
struct Pool : std::enable_shared_from_this<Pool> {
    std::shared_ptr<MppRuntime> runtime = std::make_shared<MppRuntime>();
    UL id = 0;
    explicit Pool(const PipelineConfig& c) {
        VideoFrameInfo v{};
        v.eFrameType = FRAME_TYPE_COMMON;
        v.stCommFrameInfo.u32Width = c.width;
        v.stCommFrameInfo.u32Height = c.height;
        v.stCommFrameInfo.u32Align = 16;
        v.stCommFrameInfo.ePixelFormat = MPP_PIXEL_FORMAT_NV12;
        v.stVFrame.u32PlaneNum = 2;
        if (VB_GetPicBufferSize(&v) <= 0) throw std::runtime_error("VB_GetPicBufferSize failed");
        // VB aligns the Y-plane allocation height, while the Linlon VENC DMABUF
        // import derives the UV offset from stride * visible height.  Preserve the
        // aligned total allocation capacity, but describe contiguous NV12 planes
        // with the offset expected by both V2D and VENC.  Otherwise 1080p leaves
        // an 8-row chroma gap which appears as a 16-row green band after encoding.
        const uint64_t y_size = uint64_t(v.stVFrame.u32PlaneStride[0]) * c.height;
        const uint64_t uv_size = uint64_t(v.stVFrame.u32PlaneStride[1]) * (c.height / 2);
        if (y_size + uv_size > v.stVFrame.u32TotalSize ||
            y_size > std::numeric_limits<uint32_t>::max() ||
            uv_size > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("invalid NV12 pool plane layout");
        v.stVFrame.u32PlaneSize[0] = static_cast<uint32_t>(y_size);
        v.stVFrame.u32PlaneSizeValid[0] = static_cast<uint32_t>(y_size);
        v.stVFrame.u32PlaneSize[1] = static_cast<uint32_t>(uv_size);
        v.stVFrame.u32PlaneSizeValid[1] = static_cast<uint32_t>(uv_size);
        VbPoolCfg a{};
        a.u32BufSize = v.stVFrame.u32TotalSize;
        a.u32BufCnt = c.max_frames + 3;
        a.eModId = MPP_ID_SYS;
        // V2D writes this pool through DMA and VENC reads it through DMA. Avoid a
        // cached CPU mapping that can later flush stale chroma over the DMA result.
        a.eRemapMode = VBUF_REMAP_MODE_NOCACHE;
        id = VB_CreatePool(&a);
        if (!id) throw std::runtime_error("VB_CreatePool failed");
        if (int rc = VB_SetFrameInfo(id, &v)) {
            VB_DestroyPool(id);
            id = 0;
            Check(rc, "VB_SetFrameInfo");
        }
    }
    ~Pool() {
        if (id) VB_DestroyPool(id);
    }
    MppFrame Get() {
        UL b = VB_GetBuffer(id, 20);
        if (!b) return {};
        VideoFrameInfo v{};
        if (VB_GetFrameInfo(b, &v)) {
            VB_ReleaseBuffer(b);
            return {};
        }
        return AdoptMppFrame(
            v, [](const auto& f) { VB_ReleaseBuffer(f.ulBufferId); }, shared_from_this());
    }
};
struct Encoder {
    std::shared_ptr<MppRuntime> runtime = std::make_shared<MppRuntime>();
    Resource resource;
    int id;
    bool created = false, enabled = false;
    std::shared_ptr<Pool> pool;
    PipelineConfig config;
    uint64_t sequence = 0;
    MppPixelFormat input_format;
    // Keep the external VB lease/context alive until the matching encoded output,
    // or until VENC is disabled. This also keeps imported DMA mappings alive.
    std::deque<MppFrame> in_flight;
    Encoder(const PipelineConfig& c, MppPixelFormat format)
        : resource("venc", c.encoder_id), id(c.encoder_id), config(c), input_format(format) {
        int rc = VENC_Init();
        if (rc != 0 && rc != ERR_VENC_ALREADY_INIT) Check(rc, "VENC_Init");
        VencChnAttr a{};
        a.eCodecType = MPP_STREAM_CODEC_H265;
        a.eInputPixelFormat = format;
        a.u32Width = c.width;
        a.u32Height = c.height;
        a.u32Align = 16;
        a.u32Bitrate = c.bitrate;
        a.u32FrameRate = c.fps;
        a.u32Gop = c.gop;
        a.eRcMode = VENC_RC_MODE_CBR;
        a.u32MinQp = c.min_qp;
        a.u32MaxQp = c.max_qp;
        Check(VENC_CreateChn(id, &a), "VENC_CreateChn");
        created = true;
        try {
            Check(VENC_EnableChn(id), "VENC_EnableChn");
            enabled = true;
            if (format == MPP_PIXEL_FORMAT_NV12) pool = std::make_shared<Pool>(c);
        } catch (...) {
            if (enabled) VENC_DisableChn(id);
            VENC_DestroyChn(id);
            created = false;
            throw;
        }
    }
    ~Encoder() {
        if (enabled) VENC_DisableChn(id);
        if (created) VENC_DestroyChn(id);
        in_flight.clear();
        pool.reset();
    }
    int SetBitrate(uint32_t bitrate) {
        if (!bitrate) return -1;
        VencRcAttr attr{};
        attr.enRcMode = VENC_RC_MODE_CBR;
        attr.u32BitRate = bitrate;
        attr.u32MinQp = config.min_qp;
        attr.u32MaxQp = config.max_qp;
        return VENC_SetRateControl(id, &attr);
    }
    void Send(const MppFrame& input) {
        ValidateRawFrame(input, config);
        const auto& v = input.info;
        auto& ci = v.stCommFrameInfo;
        if (ci.ePixelFormat != input_format)
            throw std::invalid_argument("raw input format changed; restart the FPV session");
        if (input_format == MPP_PIXEL_FORMAT_BGRA) {
            // Backpressure is bounded; never overwrite a submitted source surface.
            if (in_flight.size() >= config.max_frames + 3U) {
                latency::Mark("direct_input_busy", v.stVFrame.u64PTS);
                return;
            }
            for (const auto& held : in_flight)
                if (held.info.stVFrame.u64PTS == v.stVFrame.u64PTS)
                    throw std::invalid_argument("direct BGRA requires unique in-flight PTS");
            in_flight.push_back(input);
            latency::Mark("encode_submit_direct_bgra", v.stVFrame.u64PTS);
            const int rc = VENC_SendFrame(id, &v, 100);
            if (rc) in_flight.pop_back();
            Check(rc, "VENC_SendFrame BGRA");
            return;
        }
        latency::Mark("scale_begin", v.stVFrame.u64PTS);
        auto target = pool->Get();
        if (!target) return;
        VideoFrameInfo out = target.info;
        out.stVFrame.u64PTS = v.stVFrame.u64PTS;
        auto& p = config;
        V2DArea src{U16(p.roi_x), U16(p.roi_y), U16(p.roi_width ? p.roi_width : ci.u32Width),
            U16(p.roi_height ? p.roi_height : ci.u32Height)};
        V2DArea dst{0, 0, U16(p.width), U16(p.height)};
        V2DHandle job = 0;
        Check(V2D_BeginJob(&job), "V2D_BeginJob");
        int rc = V2D_AddBitblitTask(job, &v, &src, &out, &dst, V2D_CSC_MODE_BUTT);
        if (rc) {
            V2D_CancelJob(job);
            Check(rc, "V2D_AddBitblitTask");
        }
        // EndJob 无论成功与否都消费 job；失败后不得再次 CancelJob。
        Check(V2D_EndJob(job), "V2D_EndJob");
        latency::Mark("scale_done", v.stVFrame.u64PTS);
        // VENC owns a VB module ref until the actual input-done callback. Our pool
        // outlives the channel, so capture contexts may be released after the blit.
        latency::Mark("encode_submit", out.stVFrame.u64PTS);
        Check(VENC_SendFrame(id, &out, 100), "VENC_SendFrame");
    }
    EncodedFrame Get() {
        StreamBufferInfo s{};
        if (VENC_GetStream(id, &s, 10)) return {};
        const auto held = std::find_if(in_flight.begin(), in_flight.end(),
            [&](const auto& frame) { return frame.info.stVFrame.u64PTS == s.u64PTS; });
        if (held != in_flight.end()) in_flight.erase(held);
        latency::Mark("encode_done", s.u64PTS, s.u32Size);
        EncodedFrame f;
        try {
            if (!s.pu8Addr || !s.u32Size || s.u32Size > 10 * 1024 * 1024)
                throw std::runtime_error("invalid encoder stream");
            auto owned = std::make_shared<std::vector<uint8_t>>(s.pu8Addr, s.pu8Addr + s.u32Size);
            f.data = {owned, owned->data()};
            f.size = owned->size();
            f.width = config.width;
            f.height = config.height;
            f.timestamp_us = s.u64PTS;
            f.frame_number = sequence++;
            f.keyframe = s.bKeyFrame;
            // The pinned MPP GetStream contract delivers one complete encoded picture.
            for (size_t i = 0; i + 5 < f.size; ++i)
                if ((*owned)[i] == 0 && (*owned)[i + 1] == 0 && (*owned)[i + 2] == 1) {
                    auto type = ((*owned)[i + 3] >> 1) & 63;
                    if (type >= 32 && type <= 34) f.parameter_sets = true;
                }
        } catch (...) {
            VENC_ReleaseStream(id, &s);
            throw;
        }
        Check(VENC_ReleaseStream(id, &s), "VENC_ReleaseStream");
        return f;
    }
};
template <class T>
struct Queue {
    struct Item {
        T value;
        size_t bytes;
        std::chrono::steady_clock::time_point at;
    };
    std::deque<Item> q;
    size_t bytes = 0;
    bool Push(T value, size_t n, const PipelineConfig& c) {
        if (!c.max_frames || !c.max_age_ms || n > c.max_bytes) return false;
        const auto now = std::chrono::steady_clock::now();
        bool dropped = false;
        while (!q.empty() &&
            (q.size() >= c.max_frames || n > c.max_bytes - bytes ||
                now - q.front().at > std::chrono::milliseconds(c.max_age_ms))) {
            bytes -= q.front().bytes;
            q.pop_front();
            dropped = true;
        }
        q.push_back({std::move(value), n, now});
        bytes += n;
        return !dropped;
    }
    T Pop(const PipelineConfig& c, bool* expired = nullptr) {
        while (!q.empty()) {
            auto item = std::move(q.front());
            q.pop_front();
            bytes -= item.bytes;
            if (std::chrono::steady_clock::now() - item.at <=
                std::chrono::milliseconds(c.max_age_ms))
                return std::move(item.value);
            if (expired) *expired = true;
        }
        return {};
    }
    void Clear() {
        q.clear();
        bytes = 0;
    }
};
std::future<Result> Ready(Result r) {
    std::promise<Result> p;
    p.set_value(std::move(r));
    return p.get_future();
}
}  // namespace
struct FpvSession::Impl {
    std::mutex stop_mutex, life, queue_mutex, update_mutex;
    std::condition_variable wake;
    FpvConfig config;
    std::atomic<bool> running{false}, stopping{false}, updating{false}, need_keyframe{false},
        need_reconnect{false}, need_external_idr{false};
    bool used = false;
    std::atomic<uint64_t> generation{1};
    std::unique_ptr<ControlSession> control;
    std::mutex codec_mutex;
    std::function<ControlMessage(const ControlMessage&)> camera_control;
    std::unique_ptr<SkySession> sky;
    std::unique_ptr<GroundSession> ground;
    std::unique_ptr<Encoder> encoder;
    uint64_t encoder_sequence = 0;
    int input_format = -1;
    detail::RecoveryGate recovery_gate;
    size_t last_keyframe_size = 0;
    std::shared_ptr<Decoder> decoder;
    std::thread media, dispatch, update;
    Queue<MppFrame> inputs, images;
    Queue<EncodedFrame> received, encoded;
    std::deque<Result> states;
    bool receive_keyframe = true, output_keyframe = true;
    std::function<void(Result)> on_state;
    std::function<void(EncodedFrame)> on_encoded;
    std::function<void(DecodedFrame)> on_video;
    void State(StatusCode code, std::string text) {
        std::lock_guard l(queue_mutex);
        if (states.size() >= 16) states.pop_front();
        states.push_back({code, std::move(text), generation});
        wake.notify_all();
    }
    void Dispatch() {
        SessionThread marker(this);
        for (;;) {
            MppFrame image;
            EncodedFrame frame;
            std::deque<Result> events;
            {
                std::unique_lock l(queue_mutex);
                wake.wait_for(l, std::chrono::milliseconds(100), [&] {
                    return !running || need_external_idr || !states.empty() || !images.q.empty() ||
                        !encoded.q.empty();
                });
                if (!running) break;
                events.swap(states);
                image = images.Pop(config.pipeline);
                bool expired = false;
                frame = encoded.Pop(config.pipeline, &expired);
                if (expired) {
                    encoded.Clear();
                    output_keyframe = !frame.keyframe;
                    if (output_keyframe) frame = {};
                    need_keyframe = true;
                }
            }
            try {
                // 外部生产者的恢复回调不持有 life/codec/queue 锁，可尝试再次提交。
                if (need_external_idr.exchange(false) && camera_control)
                    camera_control({0xa00e, {}});
                for (auto& r : events)
                    if (on_state) on_state(r);
                if (image && on_video) {
                    latency::Mark("decode_callback", image.info.stVFrame.u64PTS);
                    on_video(std::move(image));
                }
                if (frame.data && on_encoded) on_encoded(std::move(frame));
            } catch (...) {
                State(StatusCode::Failed, "user callback threw");
            }
        }
    }
    void Deliver(EncodedFrame f) {
        if (f.channel != config.channel) return;
        f.generation = generation;
        std::lock_guard l(queue_mutex);
        if (!running) return;
        if (on_encoded) {
            if (!output_keyframe || f.keyframe) {
                if (!encoded.Push(f, f.size, config.pipeline)) {
                    encoded.Clear();
                    output_keyframe = true;
                    need_keyframe = true;
                } else if (f.keyframe)
                    output_keyframe = false;
            }
        }
        if (config.pipeline.decoded_output) {
            if (!receive_keyframe || f.keyframe) {
                if (!received.Push(f, f.size, config.pipeline)) {
                    received.Clear();
                    receive_keyframe = true;
                    need_keyframe = true;
                } else if (f.keyframe)
                    receive_keyframe = false;
            }
        }
        wake.notify_all();
    }
    ControlMessage HandleControl(const ControlMessage& m) {
        SessionThread marker(this);
        // Encoded 模式的查询、调码率和 IDR 均由外部编码器处理。
        if (config.input_mode == InputMode::Encoded && camera_control) return camera_control(m);
        std::unique_lock l(codec_mutex);
        ControlMessage r{m.tag + 1, {}};
        auto put = [](uint8_t* p, uint32_t v) {
            for (int i = 0; i < 4; ++i) p[i] = v >> (i * 8);
        };
        auto get = [](const uint8_t* p) {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
                (uint32_t(p[3]) << 24);
        };
        if (m.tag == 0xa006 && encoder) {
            r.payload.resize(92);
            put(r.payload.data() + 4, config.pipeline.width);
            put(r.payload.data() + 8, config.pipeline.height);
            put(r.payload.data() + 12, config.pipeline.fps);
            put(r.payload.data() + 16, config.pipeline.bitrate);
            return r;
        }
        int rc = -1;
        if (m.tag == 0xa00a && encoder && m.payload.size() == 92) {
            rc = encoder->SetBitrate(get(m.payload.data() + 16));
        } else if (m.tag == 0xa00e && encoder)
            rc = ForceIdrLocked();
        else if (camera_control) {
            l.unlock();
            return camera_control(m);
        }
        r.payload.resize(136);
        put(r.payload.data() + 4, rc);
        return r;
    }
    bool StartLink() {
        auto link = ResolveLink(config);
        if (config.role == Role::Sky) {
            sky = std::make_unique<SkySession>();
            return sky->Start(link);
        }
        ground = std::make_unique<GroundSession>();
        ground->OnEncodedFrame([this](auto f) { Deliver(std::move(f)); });
        ground->OnEvent([this](Event e) {
            if (e == Event::StreamInterrupted && config.link.transport == Transport::Usb)
                need_reconnect = true;
            if (e == Event::WaitingForKeyframe) {
                need_keyframe = true;
                State(StatusCode::NotReady, "waiting for keyframe");
            }
            wake.notify_all();
        });
        return ground->Start(link);
    }
    bool StartControl() {
        if (!config.control_enabled) return true;
        control = std::make_unique<ControlSession>();
        return control->Start(config, [this](const auto& m) { return HandleControl(m); });
    }
    void StopLink() {
        if (control) control->Stop();
        control.reset();
        if (sky) sky->StopSending();
        if (ground) ground->StopReceiving();
        sky.reset();
        ground.reset();
    }
    int ForceIdrLocked() {
        const auto interval = config.link.transport == Transport::Udp
            ? detail::UdpFrameTimeout(config.link, last_keyframe_size)
            : std::chrono::duration_cast<detail::FrameClock::duration>(config.link.limits.max_age);
        if (!recovery_gate.Allow(detail::FrameClock::now(), interval)) return 0;
        const int rc = VENC_SetForceIDR(encoder->id);
        if (rc != 0) recovery_gate.Reset();
        return rc;
    }
    void RequestEncoderRecovery() {
        if (encoder) {
            std::lock_guard codec(codec_mutex);
            ForceIdrLocked();
        } else if (config.role == Role::Sky && config.input_mode == InputMode::Encoded) {
            need_external_idr = true;
            wake.notify_all();
        }
    }
    void ResetCodecsForGeneration() {
        // 链路已停止；销毁旧通道，避免其迟到输出被标记成新连接代际。
        std::lock_guard codec(codec_mutex);
        recovery_gate.Reset();
        last_keyframe_size = 0;
        decoder.reset();
        if (encoder) encoder_sequence = encoder->sequence;
        encoder.reset();
        // Recreate lazily when the next frame arrives, preserving its format.
    }
    void Media() {
        SessionThread marker(this);
        auto retry_after = std::chrono::steady_clock::time_point{};
        while (running && !stopping) {
            try {
                if (updating) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                // The lifecycle lock also excludes link replacement and codec controls.
                std::unique_lock l(life, std::try_to_lock);
                if (!l.owns_lock()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                if (!running || stopping) break;
                if (updating) continue;
                if (config.link.transport == Transport::Usb &&
                    ((sky && !sky->Ready()) || need_reconnect)) {
                    if (std::chrono::steady_clock::now() < retry_after) {
                        l.unlock();
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }
                    need_reconnect = true;
                    StopLink();
                    Clear();
                    ++generation;
                    ResetCodecsForGeneration();
                    bool connected = StartLink() && StartControl();
                    if (!connected) StopLink();
                    need_reconnect = !connected;
                    retry_after = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                    State(connected ? StatusCode::Ok : StatusCode::NotReady,
                        connected ? "USB reopened; waiting for keyframe"
                                    : "USB disconnected; retrying");
                    RequestEncoderRecovery();
                    continue;
                }
                if (need_keyframe.exchange(false) && control && config.role == Role::Ground)
                    control->SendControl({0xa00e, {}});
                if (config.role == Role::Sky && config.input_mode == InputMode::Raw) {
                    if (!sky) {
                        l.unlock();
                        std::unique_lock q(queue_mutex);
                        wake.wait_for(q, std::chrono::milliseconds(100));
                        continue;
                    }
                    MppFrame f;
                    {
                        std::lock_guard q(queue_mutex);
                        f = inputs.Pop(config.pipeline);
                    }
                    std::lock_guard codec(codec_mutex);
                    if (f && f.generation == generation) {
                        if (!encoder) {
                            encoder = std::make_unique<Encoder>(
                                config.pipeline, f.info.stCommFrameInfo.ePixelFormat);
                            encoder->sequence = encoder_sequence;
                        }
                        latency::Mark("capture_ready", f.info.stVFrame.u64PTS, f.frame_number);
                        encoder->Send(f);
                    }
                    auto out = encoder ? encoder->Get() : EncodedFrame{};
                    if (out.data && out.keyframe) last_keyframe_size = out.size;
                    out.channel = config.channel;
                    out.generation = generation;
                    if (out.data && sky &&
                        sky->SubmitEncodedFrame(std::move(out)) != SubmitResult::Accepted)
                        ForceIdrLocked();
                } else if (config.role == Role::Ground && config.pipeline.decoded_output) {
                    EncodedFrame f;
                    {
                        std::lock_guard q(queue_mutex);
                        bool expired = false;
                        f = received.Pop(config.pipeline, &expired);
                        if (expired) {
                            received.Clear();
                            receive_keyframe = !f.keyframe;
                            if (receive_keyframe) f = {};
                            need_keyframe = true;
                        }
                    }
                    if (f.data) {
                        if (!decoder && !f.keyframe) {
                            need_keyframe = true;
                            continue;
                        }
                        if (!decoder)
                            decoder = std::make_shared<Decoder>(config.pipeline.decoder_id,
                                MPP_STREAM_CODEC_H265, f.width, f.height);
                        decoder->Send(f.data.get(), f.size, f.timestamp_us, MPP_STREAM_CODEC_H265);
                    }
                    if (decoder) {
                        auto f = decoder->Get();
                        if (f) {
                            std::lock_guard q(queue_mutex);
                            f.generation = generation;
                            f.channel = config.channel;
                            const auto bytes = f.info.stVFrame.u32TotalSize;
                            images.Push(std::move(f), bytes, config.pipeline);
                            wake.notify_all();
                        }
                    } else {
                        l.unlock();
                        std::unique_lock q(queue_mutex);
                        wake.wait_for(q, std::chrono::milliseconds(20), [&] {
                            return !running || stopping || updating || !received.q.empty();
                        });
                    }
                } else {
                    l.unlock();
                    // Encoded 无媒体处理；等待控制/停止信号，定时检查 USB 状态。
                    std::unique_lock q(queue_mutex);
                    auto pending = [&] {
                        return !running || stopping || need_keyframe || need_reconnect;
                    };
                    wake.wait_for(q, std::chrono::milliseconds(100), pending);
                }
                // std::mutex does not guarantee fairness. Yield outside the lifecycle
                // lock so capture submissions and lifecycle operations can progress.
                if (l.owns_lock()) {
                    l.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            } catch (const std::exception& e) {
                if (config.role == Role::Ground) {
                    std::lock_guard q(queue_mutex);
                    received.Clear();
                    receive_keyframe = true;
                    need_keyframe = true;
                }
                State(StatusCode::Failed, e.what());
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }
    void Clear() {
        std::lock_guard q(queue_mutex);
        inputs.Clear();
        images.Clear();
        received.Clear();
        encoded.Clear();
        states.clear();
        receive_keyframe = output_keyframe = true;
    }
};
void FpvSession::OnCameraControl(std::function<ControlMessage(const ControlMessage&)> cb) {
    if (callback_session == impl_.get()) throw std::logic_error("register before Start");
    std::lock_guard l(impl_->life);
    if (impl_->used) throw std::logic_error("register before Start");
    impl_->camera_control = std::move(cb);
}
std::future<ControlReply> FpvSession::SendControl(ControlMessage m) {
    std::unique_lock l(impl_->life, std::defer_lock);
    if (callback_session == impl_.get())
        l.try_lock();
    else
        l.lock();
    if (l.owns_lock() && impl_->running && !impl_->stopping && !impl_->updating && impl_->control)
        return impl_->control->SendControl(std::move(m));
    std::promise<ControlReply> p;
    p.set_value({ControlStatus::NotSent, {}});
    return p.get_future();
}
FpvSession::FpvSession() : impl_(new Impl) {}
FpvSession::~FpvSession() { Stop(); }
void FpvSession::OnState(std::function<void(Result)> cb) {
    if (callback_session == impl_.get()) throw std::logic_error("register before Start");
    std::lock_guard l(impl_->life);
    if (impl_->used) throw std::logic_error("register before Start");
    impl_->on_state = std::move(cb);
}
void FpvSession::OnEncodedFrame(std::function<void(EncodedFrame)> cb) {
    if (callback_session == impl_.get()) throw std::logic_error("register before Start");
    std::lock_guard l(impl_->life);
    if (impl_->used) throw std::logic_error("register before Start");
    impl_->on_encoded = std::move(cb);
}
void FpvSession::OnVideoFrame(std::function<void(DecodedFrame)> cb) {
    if (callback_session == impl_.get()) throw std::logic_error("register before Start");
    std::lock_guard l(impl_->life);
    if (impl_->used) throw std::logic_error("register before Start");
    impl_->on_video = std::move(cb);
}
Result FpvSession::Start(const FpvConfig& c) {
    auto& s = *impl_;
    if (callback_session == &s) return {StatusCode::NotReady, "Start from callback is prohibited"};
    std::lock_guard stop(s.stop_mutex);
    std::unique_lock l(s.life);
    if (s.used || s.stopping) return {StatusCode::NotReady, "session already started/stopped"};
    s.used = true;
    try {
        ValidateConfig(c);
        s.config = c;
        // Input format is selected by the first accepted raw frame, not by an
        // assumed NV12 default. Control/link startup does not allocate VENC yet.
        if (!s.StartLink() || !s.StartControl())
            throw std::runtime_error("link/control open/bind failed");
        s.running = true;
        s.dispatch = std::thread([&s] { s.Dispatch(); });
        s.media = std::thread([&s] { s.Media(); });
        s.State(StatusCode::Ok, "local session ready; awaiting video");
        return {};
    } catch (const std::exception& e) {
        s.running = false;
        s.stopping = true;
        s.wake.notify_all();
        l.unlock();
        if (s.media.joinable()) s.media.join();
        if (s.dispatch.joinable()) s.dispatch.join();
        l.lock();
        s.StopLink();
        s.encoder.reset();
        s.decoder.reset();
        return {StatusCode::Failed, e.what()};
    }
}
SubmitResult FpvSession::SubmitFrame(MppFrame f) {
    auto& s = *impl_;
    if (!s.running || s.stopping || s.updating) return SubmitResult::NotReady;
    std::unique_lock l(s.life, std::defer_lock);
    if (callback_session == &s)
        l.try_lock();
    else
        l.lock();
    if (!l.owns_lock() || !s.running || s.stopping || s.updating || !s.sky ||
        s.config.role != Role::Sky || s.config.input_mode != InputMode::Raw)
        return SubmitResult::NotReady;
    if (f.channel != s.config.channel || (f.generation && f.generation != s.generation))
        return SubmitResult::InvalidFrame;
    try {
        ValidateRawFrame(f, s.config.pipeline);
    } catch (...) {
        return SubmitResult::InvalidFrame;
    }
    std::lock_guard q(s.queue_mutex);
    f.generation = s.generation;
    auto size = f.info.stVFrame.u32TotalSize;
    if (size > s.config.pipeline.max_bytes) return SubmitResult::QueueFull;
    const int format = f.info.stCommFrameInfo.ePixelFormat;
    if (s.input_format >= 0 && s.input_format != format) return SubmitResult::InvalidFrame;
    s.input_format = format;
    // Raw input drops the oldest frame; Accepted means this new frame is retained.
    s.inputs.Push(std::move(f), size, s.config.pipeline);
    s.wake.notify_all();
    return SubmitResult::Accepted;
}
SubmitResult FpvSession::SubmitEncodedFrame(EncodedFrame f) {
    auto& s = *impl_;
    if (!s.running || s.stopping || s.updating) return SubmitResult::NotReady;
    std::unique_lock l(s.life, std::defer_lock);
    if (callback_session == &s)
        l.try_lock();
    else
        l.lock();
    if (!l.owns_lock() || !s.running || s.stopping || s.updating || !s.sky ||
        s.config.role != Role::Sky || s.config.input_mode != InputMode::Encoded)
        return SubmitResult::NotReady;
    if (f.channel != s.config.channel || (f.generation && f.generation != s.generation))
        return SubmitResult::InvalidFrame;
    f.generation = s.generation;
    const auto result = s.sky->SubmitEncodedFrame(std::move(f));
    if (result == SubmitResult::QueueFull || result == SubmitResult::NotReady)
        s.RequestEncoderRecovery();
    return result;
}
std::future<Result> FpvSession::UpdateConnections(Connections c) {
    auto& s = *impl_;
    if (callback_session == &s)
        return Ready({StatusCode::NotReady, "connection updates from callbacks are prohibited"});
    std::lock_guard u(s.update_mutex);
    if (!s.running || s.stopping) return Ready({StatusCode::NotReady, "session not running"});
    auto p = std::make_shared<std::promise<Result>>();
    auto result = p->get_future();
    if (s.updating.exchange(true)) return Ready({StatusCode::Busy, "update already pending"});
    if (s.update.joinable()) s.update.join();
    try {
        s.update = std::thread([&s, c = std::move(c), p] {
            Result r;
            std::lock_guard l(s.life);
            try {
                if (s.stopping)
                    r = {StatusCode::Cancelled, "stopping"};
                else {
                    auto next = s.config;
                    next.connections = c;
                    ValidateConfig(next);
                    if (next.connections.sky.device != s.config.connections.sky.device ||
                        next.connections.ground.device != s.config.connections.ground.device)
                        r = {StatusCode::RestartRequired, "device ownership cannot change online"};
                    else {
                        s.StopLink();
                        s.Clear();
                        {
                            std::lock_guard q(s.queue_mutex);
                            s.config.connections = next.connections;
                        }
                        ++s.generation;
                        s.ResetCodecsForGeneration();
                        if (!s.StartLink() || !s.StartControl()) {
                            s.StopLink();
                            s.need_reconnect = s.config.link.transport == Transport::Usb;
                            r = {
                                StatusCode::Failed, "reconnect failed; disconnected", s.generation};
                        } else {
                            s.need_reconnect = false;
                            s.RequestEncoderRecovery();
                            r = {StatusCode::Ok, "connections applied", s.generation};
                        }
                    }
                }
            } catch (const std::exception& e) {
                r = {StatusCode::InvalidConfig, e.what(), s.generation};
            }
            s.State(r.code, r.message);
            s.updating = false;
            s.wake.notify_all();
            p->set_value(r);
        });
    } catch (const std::exception& e) {
        s.updating = false;
        s.wake.notify_all();
        p->set_value({StatusCode::Failed, e.what(), s.generation});
    }
    return result;
}
void FpvSession::Stop() {
    auto& s = *impl_;
    if (callback_session == &s) throw std::logic_error("blocking Stop from callback is prohibited");
    std::lock_guard stop(s.stop_mutex);
    s.stopping = true;
    {
        std::lock_guard u(s.update_mutex);
        if (s.update.joinable()) s.update.join();
    }
    {
        std::lock_guard l(s.life);
        s.running = false;
    }
    s.wake.notify_all();
    if (s.media.joinable()) s.media.join();
    {
        std::lock_guard l(s.life);
        s.StopLink();
        s.Clear();
        std::lock_guard codec(s.codec_mutex);
        s.encoder.reset();
        s.decoder.reset();
    }
    if (s.dispatch.joinable()) s.dispatch.join();
}
}  // namespace video_transmission
