// 地端接收入口：可选 H.265 FIFO 预览与解码帧观测。
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <video_transmission/fpv_session.hpp>

#include "../../adapters/example_config.hpp"
#include "h265_preview.hpp"
#include "pts_playout.hpp"
#ifdef FPV_HAS_GUI
#include "gui.hpp"
#endif
namespace {
volatile std::sig_atomic_t exiting = 0;
void Exit(int) { exiting = 1; }
uint32_t ParseUnsigned(const char* value) {
    const std::string text(value);
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("playout options require non-negative integers");
    const auto number = std::stoull(text);
    if (number > 10000) throw std::invalid_argument("playout option exceeds limit");
    return static_cast<uint32_t>(number);
}
}  // namespace
int main(int argc, char** argv) {
    try {
        std::string config_path, fifo_path;
        bool gui_requested = false;
        bool fullscreen_requested = false;
        bool playout_requested = false;
        fpv_example::PlayoutConfig playout;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--gui")
                gui_requested = true;
            else if (arg == "--fullscreen")
                fullscreen_requested = true;
            else if (arg == "--config" && i + 1 < argc)
                config_path = argv[++i];
            else if (arg == "--preview-fifo" && i + 1 < argc)
                fifo_path = argv[++i];
            else if (arg == "--playout-delay-ms" && i + 1 < argc) {
                playout.delay_ms = ParseUnsigned(argv[++i]);
                playout_requested = true;
            } else if (arg == "--playout-max-delay-ms" && i + 1 < argc) {
                playout.max_age_ms = ParseUnsigned(argv[++i]);
                playout_requested = true;
            } else if (arg == "--playout-max-frames" && i + 1 < argc) {
                playout.max_frames = ParseUnsigned(argv[++i]);
                playout_requested = true;
            } else {
                std::cerr << "usage: fpv_ground --config path.yaml [--gui] [--fullscreen] "
                            "[--preview-fifo path]\n"
                            "  [--playout-delay-ms 250 --playout-max-delay-ms 400 "
                            "--playout-max-frames 16]\n";
                return arg == "--help" ? 0 : 2;
            }
        }
        if (config_path.empty()) throw std::runtime_error("--config is required");
        if (fullscreen_requested && !gui_requested)
            throw std::invalid_argument("--fullscreen requires --gui");
        if (playout_requested && !gui_requested)
            throw std::invalid_argument("PTS playout options require --gui");
        const fpv_example::PtsPlayout<int> validate_playout(playout);
        std::signal(SIGPIPE, SIG_IGN);
        auto config = video_transmission::example::LoadExampleConfig(config_path);
        video_transmission::example::ValidateExampleConfig(config);
        video_transmission::RequireRole(config.session, video_transmission::Role::Ground);
        if (playout.delay_ms) {
            // A four-frame decoder input queue can discard an entire GOP when the
            // paced I frame is followed by a burst of P frames. Give that stage the
            // same bounded budget; this does not enlarge the decoder's MPP pool.
            config.session.pipeline.max_frames =
                std::max(config.session.pipeline.max_frames, playout.max_frames);
            config.session.pipeline.max_age_ms =
                std::max(config.session.pipeline.max_age_ms, playout.max_age_ms);
            std::cout << "Ground receive queue: max_frames=" << config.session.pipeline.max_frames
                        << " max_age_ms=" << config.session.pipeline.max_age_ms
                        << " max_bytes=" << config.session.pipeline.max_bytes << '\n';
        }
        std::cout << video_transmission::example::DescribeExampleConfig(config) << std::endl;
        std::unique_ptr<H265Preview> preview;
        if (!fifo_path.empty()) preview = std::make_unique<H265Preview>(fifo_path);
#ifdef FPV_HAS_GUI
        std::unique_ptr<GroundGui> gui;
        if (gui_requested) {
            if (!config.session.pipeline.decoded_output)
                throw std::runtime_error("--gui requires output.kind: decoded_callback");
            gui = std::make_unique<GroundGui>(playout, fullscreen_requested);
        }
#else
        if (gui_requested)
            throw std::runtime_error("GUI unavailable: rebuild with SDL2 and FPV_BUILD_GUI=ON");
#endif
        // 异常或启动失败时，先销毁 session，避免回调访问已销毁的 preview。
        video_transmission::FpvSession session;
        session.OnState([](const auto& r) {
            std::cerr << "generation=" << r.generation << " " << r.message << '\n';
        });
        // The optional preview feeds an external player; decoded frames remain available here.
        session.OnVideoFrame([&](video_transmission::MppFrame frame) {
            if (!frame.owner) return;
            static uint64_t count = 0;
            if (++count % 120 == 0)
                std::cout << "decoded pts_us=" << frame.info.stVFrame.u64PTS << '\n';
#ifdef FPV_HAS_GUI
            if (gui) gui->Push(std::move(frame));
#endif
        });
        session.OnEncodedFrame([&preview](video_transmission::EncodedFrame frame) {
            static uint64_t count = 0;
            if (++count % 120 == 0) std::cout << "encoded bytes=" << frame.size << '\n';
            if (preview) preview->Push(std::move(frame));
        });
        auto result = session.Start(config.session);
        if (!result.ok()) {
            std::cerr << result.message << '\n';
            return 1;
        }
        std::signal(SIGINT, Exit);
        std::signal(SIGTERM, Exit);
        while (!exiting) {
#ifdef FPV_HAS_GUI
            if (gui && !gui->Poll()) break;
#endif
            if (!gui_requested) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        session.Stop();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
