#include <video_transmission/video_transmission.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace {
struct Received {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<uint8_t> bytes;
    uint64_t number = 0;
};
void OnFrame(void* user, const vt_encoded_frame* frame) {
    auto& r = *static_cast<Received*>(user);
    std::lock_guard lock(r.mutex);
    r.bytes.assign(frame->data, frame->data + frame->size);
    r.number = frame->frame_number;
    r.cv.notify_all();
}
uint16_t FreePort() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    close(fd);
    return ntohs(address.sin_port);
}
vt_session* MakeSession(vt_role role, uint16_t port, vt_callbacks callbacks) {
    vt_reference_udp_config udp{sizeof(udp), "127.0.0.1", "127.0.0.1", port, 8192};
    vt_transport_backend transport{};
    assert(vt_reference_udp_create(&udp, &transport) == VT_OK);
    vt_session_config config{sizeof(config), VT_ABI_VERSION, role, VT_INPUT_ENCODED,
                             8, 32768, 8192, 1000};
    vt_backends backends{sizeof(backends), &transport, nullptr, nullptr};
    vt_session* session = nullptr;
    assert(vt_session_create(&config, &callbacks, &backends, &session) == VT_OK);
    assert(vt_session_start(session) == VT_OK);
    return session;
}
}  // namespace

int main() {
    const auto port = FreePort();
    Received received;
    vt_callbacks ground_callbacks{sizeof(ground_callbacks), &received, nullptr, OnFrame, nullptr};
    vt_callbacks sky_callbacks{sizeof(sky_callbacks), nullptr, nullptr, nullptr, nullptr};
    auto* ground = MakeSession(VT_GROUND, port, ground_callbacks);
    auto* sky = MakeSession(VT_SKY, port, sky_callbacks);
    std::vector<uint8_t> bytes(5000);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(i * 17);
    vt_encoded_frame frame{sizeof(frame), bytes.data(), bytes.size(), 640, 400, 42, 123456,
                           VT_FRAME_KEYFRAME | VT_FRAME_PARAMETER_SETS};
    assert(vt_session_submit_encoded(sky, &frame) == VT_OK);
    assert(vt_session_drain(sky, 3000) == VT_OK);
    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, std::chrono::seconds(3),
                                    [&] { return !received.bytes.empty(); }));
        assert(received.bytes == bytes);
        assert(received.number == 42);
    }
    vt_session_destroy(sky);
    vt_session_destroy(ground);
}
