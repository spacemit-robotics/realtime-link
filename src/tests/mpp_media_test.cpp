#include <video_transmission/mpp_media.h>
#include <venc/venc_api.h>
#include <vdec/vdec_api.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
namespace {
std::atomic<int> encoded{0}, decoded{0}, released{0}, decoder_destroyed{0};
std::atomic<bool> send_ready{false}, decode_ready{false};
vt_raw_frame kept{};
void on_encoded(void*, const vt_encoded_frame* f) {
    assert(f->size == 8);
    ++encoded;
}
void on_raw(void*, const vt_raw_frame* f) {
    kept = *f;
    kept.retain(kept.native_buffer);
    ++decoded;
}
void input_release(void*, const VideoFrameInfo*) {}
uint16_t free_port() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t n = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &n) == 0);
    close(fd);
    return ntohs(address.sin_port);
}
void received_raw(void*, const vt_raw_frame* f) { on_raw(nullptr, f); }

bool wait(std::atomic<int>& v) {
    for (int i = 0; i < 200 && !v.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return v.load() != 0;
}
}  // namespace
extern "C" {
S32 VENC_CreateChn(S32, const VencChnAttr*) { return 0; }
S32 VENC_EnableChn(S32) { return 0; }
S32 VENC_DisableChn(S32) { return 0; }
S32 VENC_DestroyChn(S32) { return 0; }
S32 VENC_SetForceIDR(S32) { return 0; }
S32 VENC_SendFrame(S32, const VideoFrameInfo*, U32) {
    send_ready = true;
    return 0;
}
S32 VENC_GetStream(S32, StreamBufferInfo* s, U32) {
    if (!send_ready.exchange(false)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return ERR_VENC_NO_STREAM;
    }
    static U8 bytes[8] = {0, 0, 1, 0x40, 1, 2, 3, 4};
    s->pu8Addr = bytes;
    s->u32Size = 8;
    s->bKeyFrame = 1;
    return 0;
}
S32 VENC_ReleaseStream(S32, const StreamBufferInfo*) { return 0; }
S32 VDEC_CreateChn(S32, const VdecChnAttr*) { return 0; }
S32 VDEC_EnableChn(S32) { return 0; }
S32 VDEC_DisableChn(S32) { return 0; }
S32 VDEC_DestroyChn(S32) {
    ++decoder_destroyed;
    return 0;
}
S32 VDEC_SendStream(S32, const StreamBufferInfo*, U32) {
    decode_ready = true;
    return 0;
}
S32 VDEC_GetFrame(S32, VideoFrameInfo* f, U32) {
    if (!decode_ready.exchange(false)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return ERR_VDEC_NO_FRAME;
    }
    f->eFrameType = FRAME_TYPE_VDEC;
    f->stVdecFrameInfo.stCommFrameInfo.u32Width = 640;
    f->stCommFrameInfo.u32Height = 480;
    f->stCommFrameInfo.ePixelFormat = MPP_PIXEL_FORMAT_NV12;
    f->ulBufferId = 77;
    return 0;
}
S32 VDEC_ReleaseFrame(S32, UL id) {
    assert(id == 77);
    ++released;
    return 0;
}
}
int main() {
    vt_mpp_media_config cfg{sizeof(cfg), VT_ABI_VERSION, VT_SKY, 0, 640, 480, 30, 1000000, 30, MPP_PIXEL_FORMAT_NV12};
    vt_media_backend media{};
    vt_media_sink sink{sizeof(sink), nullptr, on_encoded, on_raw};
    assert(vt_mpp_media_create(&cfg, &media) == VT_OK);
    assert(media.start(media.context, VT_SKY, &sink) == VT_OK);
    VideoFrameInfo info{};
    info.eFrameType = FRAME_TYPE_COMMON;
    info.stCommFrameInfo.u32Width = 640;
    info.stCommFrameInfo.u32Height = 480;
    info.stCommFrameInfo.ePixelFormat = MPP_PIXEL_FORMAT_NV12;
    vt_raw_frame raw{};
    assert(vt_mpp_frame_wrap(&info, 0, input_release, nullptr, &raw) == VT_OK);
    assert(media.encode(media.context, &raw) == VT_OK);
    assert(wait(encoded));
    raw.release(raw.native_buffer);
    media.stop(media.context);
    media.destroy(media.context);
    cfg.role = VT_GROUND;
    assert(vt_mpp_media_create(&cfg, &media) == VT_OK);
    assert(media.start(media.context, VT_GROUND, &sink) == VT_OK);
    uint8_t bytes[8]{};
    vt_encoded_frame frame{sizeof(frame), bytes, sizeof(bytes), 640, 480, 0, 0, 0};
    assert(media.decode(media.context, &frame) == VT_OK);
    assert(wait(decoded));
    media.stop(media.context);
    media.destroy(media.context);
    assert(released == 0 && decoder_destroyed == 0);
    kept.release(kept.native_buffer);
    assert(released == 1 && decoder_destroyed == 1);
    // Public sessions, reference UDP, MPP-backed media adapters and a retained
    // decoded frame: codec calls are fakes, but session and UDP paths are real.
    encoded = 0;
    decoded = 0;
    released = 0;
    decoder_destroyed = 0;
    auto port = free_port();
    vt_reference_udp_config udp{sizeof(udp), "127.0.0.1", "127.0.0.1", port, 8192};
    vt_transport_backend tx{}, rx{};
    assert(vt_reference_udp_create(&udp, &tx) == VT_OK);
    assert(vt_reference_udp_create(&udp, &rx) == VT_OK);
    vt_media_backend encoder{}, decoder{};
    cfg.role = VT_SKY;
    assert(vt_mpp_media_create(&cfg, &encoder) == VT_OK);
    cfg.role = VT_GROUND;
    assert(vt_mpp_media_create(&cfg, &decoder) == VT_OK);
    vt_backends sky_backends{sizeof(vt_backends), &tx, &encoder, nullptr};
    vt_backends ground_backends{sizeof(vt_backends), &rx, &decoder, nullptr};
    vt_callbacks sky_callbacks{sizeof(vt_callbacks), nullptr, nullptr, nullptr, nullptr};
    vt_callbacks ground_callbacks{sizeof(vt_callbacks), nullptr, nullptr, nullptr, received_raw};
    vt_session_config session_cfg{
        sizeof(vt_session_config), VT_ABI_VERSION, VT_GROUND, VT_INPUT_ENCODED, 4, 2 * 1024 * 1024, 8192, 1000};
    vt_session *sky = nullptr, *ground = nullptr;
    assert(vt_session_create(&session_cfg, &ground_callbacks, &ground_backends, &ground) == VT_OK);
    session_cfg.role = VT_SKY;
    session_cfg.input_mode = VT_INPUT_RAW;
    assert(vt_session_create(&session_cfg, &sky_callbacks, &sky_backends, &sky) == VT_OK);
    assert(vt_session_start(ground) == VT_OK);
    assert(vt_session_start(sky) == VT_OK);
    assert(vt_mpp_frame_wrap(&info, 1, input_release, nullptr, &raw) == VT_OK);
    assert(vt_session_submit_raw(sky, &raw) == VT_OK);
    raw.release(raw.native_buffer);
    assert(wait(decoded));
    vt_session_destroy(sky);
    vt_session_destroy(ground);
    assert(released == 0 && decoder_destroyed == 0);
    kept.release(kept.native_buffer);
    assert(released == 1 && decoder_destroyed == 1);
}
