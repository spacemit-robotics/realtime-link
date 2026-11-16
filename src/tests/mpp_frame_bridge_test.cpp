#include <video_transmission/mpp_frame.h>
#include <cassert>
#include <atomic>
#include <thread>
#include <chrono>

namespace {
struct State {
    std::atomic<int> released{0};
    UL buffer = 0;
};
void done(void* p, const VideoFrameInfo* info) {
    auto* state = static_cast<State*>(p);
    ++state->released;
    state->buffer = info->ulBufferId;
}
vt_status transport_start(void*, vt_role, const vt_transport_sink*) { return VT_OK; }
vt_status transport_send(void*, const vt_encoded_frame*) { return VT_OK; }
vt_status media_start(void*, vt_role, const vt_media_sink*) { return VT_OK; }
vt_status media_encode(void*, const vt_raw_frame* frame) {
    const auto* info = vt_mpp_frame_info(frame);
    return info && info->ulBufferId == 42 ? VT_OK : VT_INVALID_ARGUMENT;
}
void stop(void*) {}
void destroy(void*) {}
}  // namespace
int main() {
    VideoFrameInfo info{};
    info.eFrameType = FRAME_TYPE_COMMON;
    info.stCommFrameInfo.u32Width = 640;
    info.stCommFrameInfo.u32Height = 480;
    info.stCommFrameInfo.ePixelFormat = MPP_PIXEL_FORMAT_NV12;
    info.stVFrame.u64PTS = 123;
    info.ulBufferId = 42;
    State state;
    vt_raw_frame frame{};
    assert(vt_mpp_frame_wrap(&info, 7, done, &state, &frame) == VT_OK);
    assert(frame.width == 640 && frame.height == 480 && frame.frame_number == 7 && frame.pts_us == 123);
    assert(vt_mpp_frame_info(&frame)->ulBufferId == 42);
    info.ulBufferId = 55;
    frame.retain(frame.native_buffer);
    frame.release(frame.native_buffer);
    assert(state.released == 0);
    frame.release(frame.native_buffer);
    assert(state.released == 1 && state.buffer == 42);
    // The submitter releases its reference immediately; the session retains
    // the frame until the asynchronous media backend has consumed it.
    info.ulBufferId = 42;
    State async_state;
    vt_raw_frame async_frame{};
    assert(vt_mpp_frame_wrap(&info, 8, done, &async_state, &async_frame) == VT_OK);
    vt_transport_backend transport{
        sizeof(transport), VT_ABI_VERSION, nullptr, transport_start, transport_send, stop, destroy};
    vt_media_backend media{
        sizeof(media), VT_ABI_VERSION, nullptr, media_start, media_encode, nullptr, nullptr, stop, destroy};
    vt_backends backends{sizeof(backends), &transport, &media, nullptr};
    vt_session_config config{sizeof(config), VT_ABI_VERSION, VT_SKY, VT_INPUT_RAW, 2, 2 * 1024 * 1024, 1024, 1000};
    vt_callbacks callbacks{sizeof(callbacks), nullptr, nullptr, nullptr, nullptr};
    vt_session* session = nullptr;
    assert(vt_session_create(&config, &callbacks, &backends, &session) == VT_OK);
    assert(vt_session_start(session) == VT_OK);
    assert(vt_session_submit_raw(session, &async_frame) == VT_OK);
    async_frame.release(async_frame.native_buffer);
    assert(vt_session_drain(session, 3000) == VT_OK);
    vt_session_destroy(session);
    assert(async_state.released == 1 && async_state.buffer == 42);
    info.eFrameType = FRAME_TYPE_VDEC;
    vt_raw_frame decoded_frame{};
    assert(vt_mpp_frame_wrap(&info, 9, done, &state, &decoded_frame) == VT_OK);
    decoded_frame.release(decoded_frame.native_buffer);
    info.stVFrame.u32FrameFlag = MPP_FRAME_FLAG_EOS;
    assert(vt_mpp_frame_wrap(&info, 9, done, &state, &decoded_frame) == VT_INVALID_ARGUMENT);
    vt_raw_frame invalid{};
    assert(vt_mpp_frame_wrap(nullptr, 0, done, &state, &invalid) == VT_INVALID_ARGUMENT);
    assert(vt_mpp_frame_info(&invalid) == nullptr);
}
