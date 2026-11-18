#include <video_transmission/backend_plugin.h>
#include <atomic>
#include <chrono>
#include <thread>
namespace {
std::atomic<int> controls{0}, destroyed{0};
struct T {
    vt_transport_sink sink{};
    std::atomic<bool> running{false};
    std::thread worker;
};
vt_status start(void* p, vt_role, const vt_transport_sink* sink) {
    auto& t = *static_cast<T*>(p);
    t.sink = *sink;
    t.running = true;
    t.worker = std::thread([&t] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        if (t.running) t.sink.on_event(t.sink.user, VT_EVENT_KEYFRAME_NEEDED, VT_OK);
        if (t.running) t.sink.on_event(t.sink.user, VT_EVENT_LINK_DOWN, VT_IO_ERROR);
    });
    return VT_OK;
}
vt_status send(void*, const vt_encoded_frame*) { return VT_OK; }
void stop(void* p) {
    auto& t = *static_cast<T*>(p);
    t.running = false;
    if (t.worker.joinable()) t.worker.join();
}
void destroy(void* p) {
    stop(p);
    delete static_cast<T*>(p);
    ++destroyed;
}
vt_status cs(void*, vt_role, const vt_control_sink*) { return VT_OK; }
vt_status cp(void*, const vt_control_request* r) {
    if (r->action != VT_CONTROL_REQUEST_KEYFRAME) return VT_UNSUPPORTED;
    ++controls;
    return VT_OK;
}
void noop(void*) {}
}  // namespace
extern "C" VT_API vt_status vt_backend_plugin_create_v1(
    const char*, vt_role, vt_transport_backend* t, vt_control_backend* c) {
    if (!t || !c) return VT_INVALID_ARGUMENT;
    auto* p = new T;
    *t = {sizeof(*t), VT_ABI_VERSION, p, start, send, stop, destroy};
    *c = {sizeof(*c), VT_ABI_VERSION, p, cs, cp, noop, noop};
    return VT_OK;
}
extern "C" VT_API int vt_test_controls() { return controls.load(); }
extern "C" VT_API int vt_test_destroyed() { return destroyed.load(); }
