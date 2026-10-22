#include "transport/frame_timing.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace video_transmission;  // NOLINT(build/namespaces)
using namespace std::chrono_literals;  // NOLINT(build/namespaces)

namespace {

SessionConfig LoopbackConfig() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    close(fd);
    SessionConfig config;
    config.address = "127.0.0.1";
    config.port = ntohs(address.sin_port);
    return config;
}

EncodedFrame MakeFrame(uint64_t number, size_t size, bool keyframe) {
    auto bytes = std::make_shared<std::vector<uint8_t>>(size, number % 251);
    EncodedFrame frame;
    frame.data = {bytes, bytes->data()};
    frame.size = size;
    frame.width = 1920;
    frame.height = 1080;
    frame.frame_number = number;
    frame.keyframe = keyframe;
    return frame;
}

struct Receiver {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<uint64_t> frames;
    size_t interruptions = 0;
    GroundSession ground;

    explicit Receiver(const SessionConfig& config) {
        ground.OnEncodedFrame([this](EncodedFrame frame) {
            std::lock_guard lock(mutex);
            for (size_t i = 0; i < frame.size; ++i)
                assert(frame.data.get()[i] == frame.frame_number % 251);
            frames.push_back(frame.frame_number);
            condition.notify_all();
        });
        ground.OnEvent([this](Event event) {
            std::lock_guard lock(mutex);
            if (event == Event::WaitingForKeyframe || event == Event::StreamInterrupted)
                ++interruptions;
        });
        assert(ground.Start(config));
    }

    ~Receiver() { ground.StopReceiving(); }

    void WaitFor(size_t count) {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, 3s, [&] { return frames.size() >= count; })) {
            throw std::runtime_error("expected " + std::to_string(count) + " frames, received " +
                std::to_string(frames.size()) +
                ", last=" + (frames.empty() ? "none" : std::to_string(frames.back())));
        }
    }
};

void LargeFrameRoundTrip() {
    const auto config = LoopbackConfig();
    Receiver receiver(config);
    SkySession sky;
    assert(sky.Start(config));
    assert(sky.SubmitEncodedFrame(MakeFrame(1, 1000, true)) == SubmitResult::Accepted);
    receiver.WaitFor(1);
    const auto started = std::chrono::steady_clock::now();
    assert(sky.SubmitEncodedFrame(MakeFrame(2, 285163, true)) == SubmitResult::Accepted);
    // A P-frame produced while the keyframe is in flight must not be rejected
    // simply because the keyframe takes >200 ms to serialize.
    std::this_thread::sleep_for(70ms);
    assert(sky.SubmitEncodedFrame(MakeFrame(3, 2000, false)) == SubmitResult::Accepted);
    receiver.WaitFor(3);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    sky.StopSending();
    receiver.ground.StopReceiving();
    assert(elapsed > 200ms && elapsed < 2s);
    assert((receiver.frames == std::vector<uint64_t>{1, 2, 3}));
    assert(receiver.interruptions == 0);
    std::cout << "285163-byte keyframe + following P-frame: complete, no interruption, "
                << std::chrono::duration<double, std::milli>(elapsed).count() << " ms\n";
}

void ConsecutiveLargeFramesRoundTrip() {
    auto config = LoopbackConfig();
    config.limits.max_age = 100ms;
    Receiver receiver(config);
    SkySession sky;
    assert(sky.Start(config));
    assert(sky.SubmitEncodedFrame(MakeFrame(1, 500000, true)) == SubmitResult::Accepted);
    assert(sky.SubmitEncodedFrame(MakeFrame(2, 500000, false)) == SubmitResult::Accepted);
    assert(sky.SubmitEncodedFrame(MakeFrame(3, 1000, false)) == SubmitResult::Accepted);
    receiver.WaitFor(3);
    sky.StopSending();
    receiver.ground.StopReceiving();
    assert((receiver.frames == std::vector<uint64_t>{1, 2, 3}));
    assert(receiver.interruptions == 0);
    std::cout << "Consecutive 500 KB frames plus following P-frame stay complete: PASS\n";
}

void QueueBoundsStillRecover() {
    auto config = LoopbackConfig();
    config.limits.max_frames = 2;
    Receiver receiver(config);
    SkySession sky;
    assert(sky.Start(config));
    assert(sky.SubmitEncodedFrame(MakeFrame(1, 500000, true)) == SubmitResult::Accepted);
    // At most one frame is in flight. At most two others can be queued.
    SubmitResult result = SubmitResult::Accepted;
    for (uint64_t i = 2; i <= 4 && result == SubmitResult::Accepted; ++i)
        result = sky.SubmitEncodedFrame(MakeFrame(i, 500000, false));
    assert(result == SubmitResult::QueueFull);
    assert(sky.SubmitEncodedFrame(MakeFrame(5, 1000, false)) == SubmitResult::NotReady);
    assert(sky.SubmitEncodedFrame(MakeFrame(6, 1000, true)) == SubmitResult::Accepted);
    {
        std::unique_lock lock(receiver.mutex);
        assert(receiver.condition.wait_for(
            lock, 3s, [&] { return !receiver.frames.empty() && receiver.frames.back() == 6; }));
    }
    sky.StopSending();
    receiver.ground.StopReceiving();
    std::cout << "Sustained overload remains bounded and recovers at IDR: PASS\n";
}

void HeadOfLineRoundTrip() {
    const auto config = LoopbackConfig();
    Receiver receiver(config);
    SkySession sky;
    assert(sky.Start(config));
    assert(sky.SubmitEncodedFrame(MakeFrame(1, 285163, true)) == SubmitResult::Accepted);
    assert(sky.SubmitEncodedFrame(MakeFrame(2, 127005, false)) == SubmitResult::Accepted);
    assert(sky.SubmitEncodedFrame(MakeFrame(3, 6996, false)) == SubmitResult::Accepted);
    receiver.WaitFor(3);
    sky.StopSending();
    receiver.ground.StopReceiving();
    assert((receiver.frames == std::vector<uint64_t>{1, 2, 3}));
    assert(receiver.interruptions == 0);
    std::cout << "Large I/P burst does not expire its valid following P-frame: PASS\n";
}

void StopDuringLargeFrame() {
    auto config = LoopbackConfig();
    config.rate_mbps = 0.1;
    Receiver receiver(config);
    SkySession sky;
    assert(sky.Start(config));
    assert(sky.SubmitEncodedFrame(MakeFrame(1, 285163, true)) == SubmitResult::Accepted);
    std::this_thread::sleep_for(30ms);
    const auto started = std::chrono::steady_clock::now();
    sky.StopSending();
    assert(std::chrono::steady_clock::now() - started < 250ms);
    std::cout << "Stop interrupts paced large frame promptly: PASS\n";
}

void PolicyChecks() {
    SessionConfig config;
    config.address = "127.0.0.1";
    assert(detail::ValidUdpTiming(config));
    const auto budget = detail::UdpFrameTimeout(config, 285163);
    assert(budget > 409ms && budget < 410ms);
    detail::RecoveryGate gate;
    const auto now = detail::FrameClock::time_point{};
    assert(detail::UdpQueueDeadline(config, now, {}) == now + config.limits.max_age);
    const auto nominal_finish = now + 209ms;
    const auto fixed_deadline = detail::UdpQueueDeadline(config, now + 30ms, nominal_finish);
    assert(fixed_deadline == nominal_finish + config.limits.max_age);
    assert(detail::UdpQueueDeadline(config, now + 300ms, nominal_finish) == now + 500ms);
    // Additional work changes later frames only, never the saved deadline.
    auto tail_finish = nominal_finish;
    for (int i = 0; i < 4; ++i) {
        const auto deadline = detail::UdpQueueDeadline(config, now, tail_finish);
        assert(deadline == tail_finish + config.limits.max_age);
        tail_finish += 100ms;
    }
    assert(fixed_deadline == now + 409ms);
    assert(now + 410ms > fixed_deadline);  // extra delay still expires this frame
    assert(gate.Allow(now, budget));
    assert(!gate.Allow(now + 1ms, budget));
    assert(!gate.Allow(now + 200ms, budget));
    assert(gate.Allow(now + budget, budget));
    gate.Reset();
    assert(gate.Allow(now, budget));
    std::cout << "Bounded transfer budget and coalesced IDR requests: PASS\n";
    config.rate_mbps = std::numeric_limits<double>::min();
    assert(!detail::ValidUdpTiming(config));
    SkySession sky;
    GroundSession ground;
    assert(!sky.Start(config));
    assert(!ground.Start(config));
    std::cout << "Unrepresentable transfer deadline rejected: PASS\n";
}

}  // namespace

int RunTests(int argc, char** argv) {
    if (argc > 2) throw std::invalid_argument("expected at most one test selector");
    const std::string mode = argc > 1 ? argv[1] : "--all";
    if (mode == "--policy-only") {
        PolicyChecks();
    } else if (mode == "--head-of-line") {
        HeadOfLineRoundTrip();
    } else if (mode == "--consecutive-large") {
        ConsecutiveLargeFramesRoundTrip();
    } else if (mode == "--large-frame") {
        // Uses unchanged public APIs, so this case can also exercise the old
        // library: it must fail when the keyframe is truncated after 200 ms.
        LargeFrameRoundTrip();
    } else if (mode == "--all") {
        PolicyChecks();
        LargeFrameRoundTrip();
        HeadOfLineRoundTrip();
        ConsecutiveLargeFramesRoundTrip();
        QueueBoundsStillRecover();
        StopDuringLargeFrame();
    } else {
        throw std::invalid_argument("unknown test selector: " + mode);
    }
    return 0;
}

int main(int argc, char** argv) {
    try {
        return RunTests(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "frame_timing_test: " << error.what() << '\n';
        return 1;
    }
}
