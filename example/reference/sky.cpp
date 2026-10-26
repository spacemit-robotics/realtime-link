#include "config.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Event(void*, const vt_event* event) {
    std::cerr << "event=" << event->type << " status=" << event->status << '\n';
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 5 || std::string(argv[1]) != "--config" ||
        std::string(argv[3]) != "--au-file") {
        std::cerr << "usage: video_transmission_sky_reference --config sky.yaml --au-file idr.au\n";
        return 2;
    }
    vt_session* session = nullptr;
    vt_transport_backend transport{};
    try {
        auto config = LoadReferenceConfig(argv[2], VT_SKY);
        std::ifstream file(argv[4], std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("cannot open AU file");
        const auto length = file.tellg();
        if (length <= 0 || static_cast<uint64_t>(length) > config.session.max_frame_bytes)
            throw std::invalid_argument("AU file exceeds configured frame limit or is empty");
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
            throw std::runtime_error("cannot read AU file");
        auto udp = config.Transport();
        auto rc = vt_reference_udp_create(&udp, &transport);
        if (rc != VT_OK) throw std::runtime_error("cannot create reference transport");
        const vt_callbacks callbacks{sizeof(vt_callbacks), nullptr, Event, nullptr, nullptr};
        const vt_backends backends{sizeof(vt_backends), &transport, nullptr, nullptr};
        rc = vt_session_create(&config.session, &callbacks, &backends, &session);
        if (rc != VT_OK) {
            transport.destroy(transport.context);
            throw std::runtime_error("cannot create session");
        }
        rc = vt_session_start(session);
        if (rc != VT_OK) throw std::runtime_error("cannot start session");
        const vt_encoded_frame frame{sizeof(vt_encoded_frame), bytes.data(), bytes.size(),
                                     config.width, config.height, 0, 0,
                                     VT_FRAME_KEYFRAME | VT_FRAME_PARAMETER_SETS};
        rc = vt_session_submit_encoded(session, &frame);
        if (rc != VT_OK) throw std::runtime_error("AU rejected by session");
        rc = vt_session_drain(session, 3000);
        if (rc != VT_OK) throw std::runtime_error("reference transport did not drain");
        vt_session_destroy(session);
        session = nullptr;
        std::cout << "submitted one AU, bytes=" << bytes.size() << '\n';
        return 0;
    } catch (const std::exception& error) {
        vt_session_destroy(session);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
