// 原生空端示例：显式采集、MJPEG 预处理和有界提交。
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <video_transmission/fpv_session.hpp>
#include <video_transmission/version.hpp>

#include "../../adapters/camera_adapter.hpp"
#include "../../adapters/example_config.hpp"

namespace {
volatile std::sig_atomic_t exiting = 0;
void Exit(int) { exiting = 1; }
}  // namespace

int main(int argc, char** argv) {
    try {
        if ((argc != 3 && argc != 5) || std::string(argv[1]) != "--config" ||
            (argc == 5 && std::string(argv[3]) != "--control-port")) {
            std::cerr << "usage: fpv_sky --config path.yaml [--control-port port]\n";
            return 2;
        }
        namespace example = video_transmission::example;
        using video_transmission::ControlMessage;
        using video_transmission::FpvSession;
        using video_transmission::InputMode;
        using video_transmission::RequireRole;
        using video_transmission::Result;
        using video_transmission::Role;
        using video_transmission::SubmitResult;
        auto config = example::LoadExampleConfig(argv[2]);
        if (argc == 5) {
            const std::string value(argv[4]);
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                throw std::invalid_argument("invalid control port");
            const auto port = std::stoul(value);
            if (port == 0 || port > 65535) throw std::invalid_argument("invalid control port");
            config.session.control_port = static_cast<uint16_t>(port);
        }
        RequireRole(config.session, Role::Sky);
        std::cout << example::DescribeExampleConfig(config) << std::endl;
        // 独立成行：每次运行都必须能确认二进制身份，便于对照测量结果。
        std::cout << "build " << video_transmission::BuildInfo() << std::endl;
        // 本例是相机生产者；外部 H.265 生产者应调用 SubmitEncodedFrame。
        if (config.session.input_mode != InputMode::Raw)
            throw std::invalid_argument("fpv_sky camera example requires input_mode=raw");
        example::CameraAdapter camera(config);
        FpvSession session;
        session.OnState([](const Result& result) {
            std::cerr << "generation=" << result.generation << " " << result.message << '\n';
        });
        session.OnCameraControl(
            [&camera](const ControlMessage& message) { return camera.HandleControl(message); });
        const auto result = session.Start(config.session);
        if (!result.ok()) throw std::runtime_error(result.message);
        std::signal(SIGINT, Exit);
        std::signal(SIGTERM, Exit);
        uint64_t dropped = 0;
        uint64_t empty = 0;
        uint64_t received = 0;
        try {
            while (!exiting) {
                auto frame = camera.Get(std::chrono::milliseconds(100));
                if (!frame) {
                    if (++empty % 50 == 0) std::cerr << "camera empty polls=" << empty << '\n';
                    continue;
                }
                if (++received <= 3 || received % 120 == 0)
                    std::cerr << "camera decoded frames=" << received << '\n';
                frame.generation = 0;
                const auto submitted = session.SubmitFrame(std::move(frame));
                if (submitted == SubmitResult::InvalidFrame)
                    throw std::runtime_error("pipeline rejected capture frame layout/channel");
                if (submitted != SubmitResult::Accepted) {
                    if (++dropped % 120 == 1)
                        std::cerr << "capture frames rejected=" << dropped << '\n';
                    if (submitted == SubmitResult::NotReady)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        } catch (...) {
            camera.Stop();
            session.Stop();
            throw;
        }
        // 生产循环在主线程，退出循环即完成生产同步；随后停 session，最后销毁设备。
        camera.Stop();
        session.Stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
