#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <video_transmission/video_transmission.hpp>

#include "legacy_udp.hpp"

using namespace video_transmission;  // NOLINT(build/namespaces)

int main() {
    legacy_udp::Header h;
    h.width = 640;
    h.height = 400;
    h.frame_number = 0x100000002ULL;
    h.timestamp_us = 0x300000004ULL;
    h.frame_len = 2000;
    h.total_packets = 2;
    h.flags = 1;
    auto wire = legacy_udp::Encode(h);
    assert(wire.size() == 128 && wire[0] == 0xfa && wire[52] == 3);
    legacy_udp::Header decoded;
    assert(legacy_udp::Decode(wire.data(), wire.size(), &decoded));
    assert(decoded.frame_number == h.frame_number && decoded.timestamp_us == h.timestamp_us);

    auto invalid_wire = wire;
    invalid_wire[52] = 4;
    assert(!legacy_udp::Decode(invalid_wire.data(), invalid_wire.size(), &decoded));
    invalid_wire = wire;
    invalid_wire[68] = 2;
    assert(!legacy_udp::Decode(invalid_wire.data(), invalid_wire.size(), &decoded));

    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    assert(probe >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const uint16_t port = ntohs(addr.sin_port);
    close(probe);

    SessionConfig config;
    config.address = "127.0.0.1";
    config.port = port;
    config.limits.max_frame_bytes = 4096;
    config.limits.max_bytes = 8192;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<uint8_t> received;
    GroundSession ground;
    ground.OnEncodedFrame([&](EncodedFrame frame) {
        std::lock_guard<std::mutex> lock(mutex);
        received.assign(frame.data.get(), frame.data.get() + frame.size);
        assert(frame.width == 640 && frame.height == 400 && frame.keyframe);
        cv.notify_all();
    });
    assert(ground.Start(config));

    SkySession sky;
    assert(sky.SubmitEncodedFrame({}) == SubmitResult::NotReady);
    assert(sky.Start(config));
    assert(sky.SubmitEncodedFrame({}) == SubmitResult::InvalidFrame);
    auto owner = std::make_shared<std::vector<uint8_t>>(2000, 0x5a);
    EncodedFrame frame;
    frame.data = {owner, owner->data()};
    frame.size = owner->size();
    frame.width = 640;
    frame.height = 400;
    frame.keyframe = true;
    frame.frame_number = h.frame_number;
    frame.timestamp_us = h.timestamp_us;
    assert(sky.SubmitEncodedFrame(frame) == SubmitResult::Accepted);
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(2), [&] { return !received.empty(); }));
        assert(received == *owner);
    }
    // RGB keyframe must not unlock an independent IR stream's P-frames.
    frame.channel = 1;
    frame.keyframe = false;
    assert(sky.SubmitEncodedFrame(frame) == SubmitResult::NotReady);
    sky.StopSending();

    {
        std::lock_guard<std::mutex> lock(mutex);
        received.clear();
    }
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    assert(sender >= 0);
    auto send_slice = [&](uint32_t index, uint8_t value) {
        legacy_udp::Header slice;
        slice.width = 640;
        slice.height = 400;
        slice.frame_number = h.frame_number + 1;
        slice.timestamp_us = h.timestamp_us + 1;
        slice.frame_len = 1;
        slice.frame_type = 0;
        slice.total_packets = 1;
        slice.slice_index = index;
        slice.slice_count = 2;
        slice.end_of_frame = index == 1;
        auto bytes = legacy_udp::Encode(slice);
        std::vector<uint8_t> packet(bytes.begin(), bytes.end());
        packet.push_back(value);
        assert(sendto(sender, packet.data(), packet.size(), 0, reinterpret_cast<sockaddr*>(&addr),
                    sizeof(addr)) == static_cast<ssize_t>(packet.size()));
    };
    send_slice(1, 0x22);
    send_slice(0, 0x11);
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(2), [&] { return received.size() == 2; }));
        assert(received[0] == 0x11 && received[1] == 0x22);
    }
    auto send_one = [&](uint64_t frame_number, uint8_t type, uint8_t value) {
        legacy_udp::Header one;
        one.width = 640;
        one.height = 400;
        one.frame_number = frame_number;
        one.frame_len = 1;
        one.frame_type = type;
        one.total_packets = 1;
        auto bytes = legacy_udp::Encode(one);
        std::vector<uint8_t> packet(bytes.begin(), bytes.end());
        packet.push_back(value);
        assert(sendto(sender, packet.data(), packet.size(), 0, reinterpret_cast<sockaddr*>(&addr),
                    sizeof(addr)) == static_cast<ssize_t>(packet.size()));
    };
    legacy_udp::Header incomplete;
    incomplete.width = 640;
    incomplete.height = 400;
    incomplete.frame_number = h.frame_number + 2;
    incomplete.frame_len = static_cast<uint32_t>(legacy_udp::kPayloadBytes + 1);
    incomplete.frame_type = 1;
    incomplete.total_packets = 2;
    auto bytes = legacy_udp::Encode(incomplete);
    std::vector<uint8_t> packet(bytes.begin(), bytes.end());
    packet.resize(legacy_udp::kPacketBytes, 0x33);
    assert(sendto(sender, packet.data(), packet.size(), 0, reinterpret_cast<sockaddr*>(&addr),
                sizeof(addr)) == static_cast<ssize_t>(packet.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    send_one(h.frame_number + 3, 1, 0x44);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::lock_guard<std::mutex> lock(mutex);
        assert(received.size() == 2);
    }
    send_one(h.frame_number + 4, 0, 0x55);
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(2),
            [&] { return received.size() == 1 && received[0] == 0x55; }));
    }
    close(sender);
    ground.StopReceiving();
}
