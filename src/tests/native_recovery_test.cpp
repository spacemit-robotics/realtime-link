#include "../../example/native/public/common.hpp"
#include <cassert>
extern "C" vt_status vt_mpp_media_create(const vt_mpp_media_config* cfg, vt_media_backend* out);
namespace {
vt_status ms(void*, vt_role, const vt_media_sink*) { return VT_OK; }
vt_status me(void*, const vt_raw_frame*) { return VT_OK; }
vt_status md(void*, const vt_encoded_frame*) { return VT_OK; }
void noop(void*) {}
}  // namespace
extern "C" vt_status vt_mpp_media_create(const vt_mpp_media_config*, vt_media_backend* out) {
    *out = {sizeof(*out), VT_ABI_VERSION, nullptr, ms, me, md, nullptr, noop, noop};
    return VT_OK;
}
int main(int argc, char** argv) {
    NativeRequestRetry retry;
    const auto now = NativeRequestRetry::Clock::now();
    retry.signal();
    assert(retry.ready(now));
    retry.finish(VT_IO_ERROR, now);
    assert(!retry.ready(now));
    assert(retry.ready(now + std::chrono::seconds(1)));
    retry.finish(VT_OK, now + std::chrono::seconds(1));
    assert(!retry.pending);
    assert(argc == 2);
    NativePlugin plugin(argv[1]);
    NativeEvents events;
    NativeSession session;
    auto observer = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    assert(observer);
    auto controls = reinterpret_cast<int (*)()>(dlsym(observer, "vt_test_controls"));
    auto destroyed = reinterpret_cast<int (*)()>(dlsym(observer, "vt_test_destroyed"));
    assert(controls && destroyed);
    for (int epoch = 1; epoch <= 2; epoch++) {
        assert(session.open(plugin, "unused", VT_GROUND, events, 16, 16, 1, 1, 1, nullptr) == VT_OK);
        for (int i = 0; i < 100 && (!events.keyframe || !events.reconnect); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(events.keyframe && events.reconnect);
        assert(request_keyframe(session) == VT_OK);
        session.reset();
        assert(destroyed() == epoch);
    }
    assert(controls() == 2);
    dlclose(observer);
}
