#include <video_transmission/video_transmission.hpp>

#include <atomic>
#include <cassert>
#include <cstdint>

namespace {
struct Buffer {
    std::atomic<int> retains{0};
    std::atomic<int> releases{0};
};
void Retain(void* ptr) { ++static_cast<Buffer*>(ptr)->retains; }
void Release(void* ptr) { ++static_cast<Buffer*>(ptr)->releases; }

struct Mock {
    vt_media_sink sink{};
    std::atomic<int> encoded{0};
    std::atomic<int> sent{0};
};
vt_status StartTransport(void*, vt_role role, const vt_transport_sink*) {
    return role == VT_SKY ? VT_OK : VT_INVALID_ARGUMENT;
}
vt_status Send(void* ptr, const vt_encoded_frame* frame) {
    auto* mock = static_cast<Mock*>(ptr);
    if (frame->size != 4 || frame->data[0] != 0x26) return VT_INVALID_ARGUMENT;
    ++mock->sent;
    return VT_OK;
}
void Stop(void*) {}
void Destroy(void*) {}
vt_status StartMedia(void* ptr, vt_role, const vt_media_sink* sink) {
    static_cast<Mock*>(ptr)->sink = *sink;
    return VT_OK;
}
vt_status Encode(void* ptr, const vt_raw_frame* raw) {
    auto* mock = static_cast<Mock*>(ptr);
    ++mock->encoded;
    const uint8_t bytes[] = {0x26, 1, 2, 3};
    const vt_encoded_frame frame{sizeof(frame), bytes, sizeof(bytes), raw->width, raw->height,
                                 raw->frame_number, raw->pts_us, VT_FRAME_KEYFRAME};
    mock->sink.on_encoded(mock->sink.user, &frame);
    return VT_OK;
}
}  // namespace

int main() {
    Mock mock;
    vt_transport_backend transport{sizeof(transport), VT_ABI_VERSION, &mock,
                                   StartTransport, Send, Stop, Destroy};
    vt_media_backend media{sizeof(media), VT_ABI_VERSION, &mock, StartMedia, Encode,
                           nullptr, nullptr, Stop, Destroy};
    vt_backends backends{sizeof(backends), &transport, &media, nullptr};
    vt_session_config config{sizeof(config), VT_ABI_VERSION, VT_SKY, VT_INPUT_RAW,
                             2, 4096, 1024, 1000};
    vt_callbacks callbacks{sizeof(callbacks), nullptr, nullptr, nullptr, nullptr};
    vt_session* session = nullptr;
    assert(vt_session_create(&config, &callbacks, &backends, &session) == VT_OK);
    assert(vt_session_start(session) == VT_OK);

    Buffer buffer;
    vt_raw_frame raw{sizeof(raw), &buffer, Retain, Release, 1, 16, 16, 7, 42};
    assert(vt_session_submit_raw(session, &raw) == VT_OK);
    // The first drain covers the queued raw frame. The second covers the AU
    // emitted by the mock encoder from the send worker.
    assert(vt_session_drain(session, 3000) == VT_OK);
    assert(vt_session_drain(session, 3000) == VT_OK);
    vt_session_destroy(session);
    assert(mock.encoded == 1);
    assert(mock.sent == 1);
    assert(buffer.retains == 1);
    assert(buffer.releases == 1);
}
