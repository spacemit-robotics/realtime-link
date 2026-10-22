#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <filesystem>  // NOLINT(build/c++17)
#include <thread>
#include <video_transmission/video_transmission.hpp>
using namespace video_transmission;  // NOLINT(build/namespaces)
int main(int argc, char** argv) {
    assert(argc == 2);
    auto config = LoadConfig((std::filesystem::path(argv[1]) / "ground.yaml").string());
    config.control_enabled = false;
    config.pipeline.decoded_output = false;
    config.connections.sky.device_ip = "127.0.0.1";
    config.connections.ground.device_ip = "127.0.0.1";
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(probe, (sockaddr*)&a, sizeof(a)) == 0);
    socklen_t len = sizeof(a);
    getsockname(probe, (sockaddr*)&a, &len);
    config.link.port = ntohs(a.sin_port);
    close(probe);
    VideoTransmission ground;
    std::mutex mutex;
    std::condition_variable cv;
    size_t received = 0;
    EncodedFrame held;
    ground.OnEncodedFrame([&](EncodedFrame f) {
        std::lock_guard l(mutex);
        ++received;
        held = std::move(f);
        cv.notify_all();
    });
    assert(ground.Start(config).ok());
    SessionConfig send = ResolveLink(config);
    send.peer.clear();
    SkySession sky;
    assert(sky.Start(send));
    auto bytes = std::make_shared<std::vector<uint8_t>>(64, 42);
    EncodedFrame frame;
    frame.data = {bytes, bytes->data()};
    frame.size = bytes->size();
    frame.width = 640;
    frame.height = 400;
    frame.keyframe = true;
    assert(sky.SubmitEncodedFrame(frame) == SubmitResult::Accepted);
    {
        std::unique_lock l(mutex);
        assert(cv.wait_for(l, std::chrono::seconds(2), [&] { return received == 1; }));
    }
    auto invalid = config.connections;
    invalid.ground.device_ip = "bad";
    assert(ground.UpdateConnections(invalid).get().code == StatusCode::InvalidConfig);
    auto update = ground.UpdateConnections(config.connections);
    assert(update.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert(update.get().ok());
    frame.frame_number = 1;
    assert(sky.SubmitEncodedFrame(frame) == SubmitResult::Accepted);
    {
        std::unique_lock l(mutex);
        assert(cv.wait_for(l, std::chrono::seconds(2), [&] { return received == 2; }));
    }
    sky.StopSending();
    std::thread stop_a([&] { ground.Stop(); });
    std::thread stop_b([&] { ground.Stop(); });
    stop_a.join();
    stop_b.join();
    ground.Stop();
    assert(held.data && held.data.get()[0] == 42);
    assert(!ground.Start(config).ok());
    assert(ground.UpdateConnections(config.connections).get().code == StatusCode::NotReady);
    // Reference-only frames share one release action and preserve their owner.
    int releases = 0;
    auto owner = std::make_shared<int>(7);
    std::weak_ptr<int> weak = owner;
    VideoFrameInfo v{};
    auto f = AdoptMppFrame(v, [&](const auto&) { ++releases; }, owner);
    owner.reset();
    {
        auto copy = f;
        f = {};
        assert(!weak.expired() && releases == 0);
    }
    assert(weak.expired() && releases == 1);
    // 地端不校验编码器尺寸；无效队列预算应返回失败，并允许安全停止。
    VideoTransmission failed;
    auto bad = config;
    bad.pipeline.max_frames = 0;
    assert(!failed.Start(bad).ok());
    failed.Stop();
}
