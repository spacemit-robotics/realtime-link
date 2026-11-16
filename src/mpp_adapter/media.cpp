#include <video_transmission/mpp_media.h>
#include <venc/venc_api.h>
#include <vdec/vdec_api.h>
#include <atomic>
#include <memory>
#include <new>
#include <thread>

namespace {
struct Decoder {
    int channel;
    bool created = false, enabled = false;
    ~Decoder() {
        if (enabled) VDEC_DisableChn(channel);
        if (created) VDEC_DestroyChn(channel);
    }
};
struct Lease {
    std::shared_ptr<Decoder> decoder;
};
void release_decoded(void* p, const VideoFrameInfo* info) {
    std::unique_ptr<Lease> lease(static_cast<Lease*>(p));
    VDEC_ReleaseFrame(lease->decoder->channel, info->ulBufferId);
}
struct Media {
    vt_mpp_media_config config{};
    vt_media_sink sink{};
    std::atomic<bool> running{false};
    std::thread worker;
    bool encoder_created = false, encoder_enabled = false, used = false;
    std::shared_ptr<Decoder> decoder;
    uint64_t sequence = 0;
    void encode_loop() {
        while (running) {
            StreamBufferInfo stream{};
            if (VENC_GetStream(config.channel_id, &stream, 20)) continue;
            if (stream.pu8Addr && stream.u32Size) {
                uint32_t flags = stream.bKeyFrame ? VT_FRAME_KEYFRAME : 0;
                for (size_t i = 0; i + 4 < stream.u32Size; ++i) {
                    if (stream.pu8Addr[i] == 0 && stream.pu8Addr[i + 1] == 0 && stream.pu8Addr[i + 2] == 1) {
                        auto type = (stream.pu8Addr[i + 3] >> 1) & 63;
                        if (type >= 32 && type <= 34) flags |= VT_FRAME_PARAMETER_SETS;
                    }
                }
                vt_encoded_frame f{sizeof(f), stream.pu8Addr, stream.u32Size, config.width, config.height, sequence++,
                    stream.u64PTS, flags};
                if (running && sink.on_encoded) sink.on_encoded(sink.user, &f);
            }
            VENC_ReleaseStream(config.channel_id, &stream);
        }
    }
    void decode_loop() {
        while (running) {
            VideoFrameInfo info{};
            if (VDEC_GetFrame(config.channel_id, &info, 20)) continue;
            auto lease = std::unique_ptr<Lease>(new (std::nothrow) Lease{decoder});
            if (!lease) {
                VDEC_ReleaseFrame(config.channel_id, info.ulBufferId);
                continue;
            }
            vt_raw_frame frame{};
            if (vt_mpp_frame_wrap(&info, sequence++, release_decoded, lease.get(), &frame) != VT_OK) {
                VDEC_ReleaseFrame(config.channel_id, info.ulBufferId);
                continue;
            }
            lease.release();
            if (running && sink.on_raw) sink.on_raw(sink.user, &frame);
            frame.release(frame.native_buffer);
        }
    }
    void stop() {
        running = false;
        if (worker.joinable()) worker.join();
        if (encoder_enabled) VENC_DisableChn(config.channel_id);
        if (encoder_created) VENC_DestroyChn(config.channel_id);
        encoder_enabled = encoder_created = false;
        decoder.reset();
    }
    ~Media() { stop(); }
};
vt_status start(void* ptr, vt_role role, const vt_media_sink* sink) {
    if (!ptr || !sink || sink->struct_size < sizeof(*sink)) return VT_INVALID_ARGUMENT;
    auto& m = *static_cast<Media*>(ptr);
    if (m.used || m.config.role != role) return VT_NOT_READY;
    m.used = true;
    m.sink = *sink;
    try {
        if (role == VT_SKY) {
            VencChnAttr attr{};
            attr.eCodecType = MPP_STREAM_CODEC_H265;
            attr.eInputPixelFormat = static_cast<MppPixelFormat>(m.config.pixel_format);
            attr.u32Width = m.config.width;
            attr.u32Height = m.config.height;
            attr.u32FrameRate = m.config.fps;
            attr.u32Bitrate = m.config.bitrate;
            attr.u32Gop = m.config.gop;
            attr.u32Align = 16;
            attr.eRcMode = VENC_RC_MODE_CBR;
            if (VENC_CreateChn(m.config.channel_id, &attr)) return VT_IO_ERROR;
            m.encoder_created = true;
            if (VENC_EnableChn(m.config.channel_id)) {
                m.stop();
                return VT_IO_ERROR;
            }
            m.encoder_enabled = true;
        } else {
            m.decoder = std::make_shared<Decoder>();
            m.decoder->channel = m.config.channel_id;
            VdecChnAttr attr{};
            attr.eCodecType = MPP_STREAM_CODEC_H265;
            attr.eOutputPixelFormat = static_cast<MppPixelFormat>(m.config.pixel_format);
            attr.eOutputMode = VDEC_OUTPUT_PULL;
            attr.u32Width = m.config.width;
            attr.u32Height = m.config.height;
            attr.u32Align = 16;
            if (VDEC_CreateChn(m.config.channel_id, &attr)) {
                m.stop();
                return VT_IO_ERROR;
            }
            m.decoder->created = true;
            if (VDEC_EnableChn(m.config.channel_id)) {
                m.stop();
                return VT_IO_ERROR;
            }
            m.decoder->enabled = true;
        }
        m.running = true;
        m.worker = std::thread([&m] {
            if (m.config.role == VT_SKY)
                m.encode_loop();
            else
                m.decode_loop();
        });
        return VT_OK;
    } catch (...) {
        m.stop();
        return VT_IO_ERROR;
    }
}
vt_status encode(void* p, const vt_raw_frame* frame) {
    if (!p || !frame) return VT_INVALID_ARGUMENT;
    auto& m = *static_cast<Media*>(p);
    if (!m.running || m.config.role != VT_SKY) return VT_NOT_READY;
    auto* info = vt_mpp_frame_info(frame);
    if (!info || frame->width != m.config.width || frame->height != m.config.height ||
        frame->pixel_format != m.config.pixel_format)
        return VT_INVALID_ARGUMENT;
    return VENC_SendFrame(m.config.channel_id, info, 100) == 0 ? VT_OK : VT_IO_ERROR;
}
vt_status decode(void* p, const vt_encoded_frame* frame) {
    if (!p || !frame || !frame->data || !frame->size || frame->size > UINT32_MAX) return VT_INVALID_ARGUMENT;
    auto& m = *static_cast<Media*>(p);
    if (!m.running || m.config.role != VT_GROUND) return VT_NOT_READY;
    StreamBufferInfo stream{};
    stream.pu8Addr = frame->data;
    stream.u32Size = frame->size;
    stream.u64PTS = frame->pts_us;
    stream.eCodecType = MPP_STREAM_CODEC_H265;
    return VDEC_SendStream(m.config.channel_id, &stream, 100) == 0 ? VT_OK : VT_IO_ERROR;
}
vt_status keyframe(void* p) {
    auto* m = static_cast<Media*>(p);
    return m && m->running && m->config.role == VT_SKY && !VENC_SetForceIDR(m->config.channel_id)
        ? VT_OK : VT_NOT_READY;
}
void stop(void* p) {
    if (p) static_cast<Media*>(p)->stop();
}
void destroy(void* p) { delete static_cast<Media*>(p); }
}  // namespace
extern "C" VT_API vt_status vt_mpp_media_create(const vt_mpp_media_config* cfg, vt_media_backend* out) {
    if (!out) return VT_INVALID_ARGUMENT;
    *out = {};
    if (!cfg || cfg->struct_size < sizeof(*cfg) || cfg->abi_version != VT_ABI_VERSION ||
        (cfg->role != VT_SKY && cfg->role != VT_GROUND) || cfg->channel_id < 0 || !cfg->width || !cfg->height ||
        (cfg->role == VT_SKY && (!cfg->fps || !cfg->bitrate || !cfg->gop)))
        return VT_INVALID_ARGUMENT;
    auto* m = new (std::nothrow) Media;
    if (!m) return VT_IO_ERROR;
    m->config = *cfg;
    *out = {sizeof(*out), VT_ABI_VERSION, m, start, encode, decode, keyframe, stop, destroy};
    return VT_OK;
}
