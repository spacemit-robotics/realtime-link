#include "config.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
volatile std::sig_atomic_t exiting = 0;
void Exit(int) { exiting = 1; }
struct Output {
    std::string path;
    std::atomic<bool> received{false}, failed{false};
};
void Event(void*, const vt_event* event) {
    std::cerr << "event=" << event->type << " status=" << event->status << '\n';
}
void Frame(void* user, const vt_encoded_frame* frame) {
    auto* output = static_cast<Output*>(user);
    std::ofstream file(output->path, std::ios::binary);
    if (!file || !file.write(reinterpret_cast<const char*>(frame->data), frame->size))
        output->failed = true;
    else output->received = true;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 6 || std::string(argv[1]) != "--config" ||
        std::string(argv[3]) != "--output" || std::string(argv[5]) != "--once") {
        std::cerr << "usage: video_transmission_ground_reference --config ground.yaml "
                     "--output received.au --once\n";
        return 2;
    }
    vt_session* session = nullptr;
    vt_transport_backend transport{};
    try {
        auto config = LoadReferenceConfig(argv[2], VT_GROUND);
        auto udp = config.Transport();
        auto rc = vt_reference_udp_create(&udp, &transport);
        if (rc != VT_OK) throw std::runtime_error("cannot create reference transport");
        Output output{argv[4]};
        const vt_callbacks callbacks{sizeof(vt_callbacks), &output, Event, Frame, nullptr};
        const vt_backends backends{sizeof(vt_backends), &transport, nullptr, nullptr};
        rc = vt_session_create(&config.session, &callbacks, &backends, &session);
        if (rc != VT_OK) {
            transport.destroy(transport.context);
            throw std::runtime_error("cannot create session");
        }
        rc = vt_session_start(session);
        if (rc != VT_OK) throw std::runtime_error("cannot start session");
        std::signal(SIGINT, Exit);
        std::signal(SIGTERM, Exit);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!exiting && !output.received && !output.failed &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        vt_session_destroy(session);
        session = nullptr;
        if (!output.received || output.failed) throw std::runtime_error("no complete AU received");
        std::cout << "received one complete AU\n";
        return 0;
    } catch (const std::exception& error) {
        vt_session_destroy(session);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
