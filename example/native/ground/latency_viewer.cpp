// A measurement viewer: one hardware decode, latest-frame mailbox, actual
// Wayland presentation feedback. No encoded FIFO / second software decoder.
#include <SDL.h>
#include <SDL_syswm.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <wayland-client.h>

#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <video_transmission/legacy_api.hpp>

#include "../../adapters/example_config.hpp"
#include "presentation-time-client-protocol.h"
namespace lat = video_transmission::latency;
namespace {
volatile std::sig_atomic_t exiting = 0;
void Exit(int) { exiting = 1; }
void SdlCheck(int rc) {
    if (rc < 0) throw std::runtime_error(SDL_GetError());
}
class Presentation {
    struct Pending {
        Presentation* owner;
        uint64_t pts, requested_us;
    };
    wl_display* display_ = nullptr;
    wl_surface* surface_ = nullptr;
    wl_event_queue* queue_ = nullptr;
    wl_registry* registry_ = nullptr;
    wp_presentation* presentation_ = nullptr;
    uint32_t clock_ = UINT32_MAX;
    std::map<struct wp_presentation_feedback*, Pending*> pending_;
    static void Clock(void* data, wp_presentation*, uint32_t clock) {
        static_cast<Presentation*>(data)->clock_ = clock;
    }
    static void Global(
        void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
        auto* self = static_cast<Presentation*>(data);
        if (std::strcmp(interface, "wp_presentation") != 0) return;
        self->presentation_ = static_cast<wp_presentation*>(
            wl_registry_bind(registry, name, &wp_presentation_interface, 1));
        static const wp_presentation_listener listener{Clock};
        wp_presentation_add_listener(self->presentation_, &listener, self);
    }
    static void Remove(void*, wl_registry*, uint32_t) {}
    static void Output(void*, struct wp_presentation_feedback*, wl_output*) {}
    static void Presented(void* data, struct wp_presentation_feedback* f, uint32_t hi, uint32_t lo,
        uint32_t ns, uint32_t, uint32_t, uint32_t, uint32_t flags) {
        auto* p = static_cast<Pending*>(data);
        const uint64_t us = ((uint64_t(hi) << 32) | lo) * 1000000 + ns / 1000;
        const auto received = lat::NowUs();
        lat::Mark("gui_feedback_received", p->pts, flags, received);
        lat::Mark("gui_presented_raw", p->pts, flags, us);
        if (us >= p->requested_us && us <= received)
            lat::Mark("gui_presented", p->pts, flags, us);
        else
            lat::Mark("gui_timestamp_invalid", p->pts, us, received);
        p->owner->pending_.erase(f);
        wp_presentation_feedback_destroy(f);
        delete p;
    }
    static void Discarded(void* data, struct wp_presentation_feedback* f) {
        auto* p = static_cast<Pending*>(data);
        lat::Mark("gui_discarded", p->pts);
        p->owner->pending_.erase(f);
        wp_presentation_feedback_destroy(f);
        delete p;
    }

public:
    explicit Presentation(SDL_Window* window) {
        SDL_SysWMinfo info{};
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WAYLAND)
            throw std::runtime_error("latency viewer requires SDL Wayland backend");
        display_ = info.info.wl.display;
        surface_ = info.info.wl.surface;
        queue_ = wl_display_create_queue(display_);
        auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display_));
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue_);
        registry_ = wl_display_get_registry(wrapper);
        wl_proxy_wrapper_destroy(wrapper);
        static const wl_registry_listener listener{Global, Remove};
        wl_registry_add_listener(registry_, &listener, this);
        wl_display_roundtrip_queue(display_, queue_);
        wl_display_roundtrip_queue(display_, queue_);
        if (!presentation_ || clock_ != CLOCK_MONOTONIC)
            throw std::runtime_error("compositor has no CLOCK_MONOTONIC presentation feedback");
        std::cerr << "GUI measurement: Wayland presentation feedback, CLOCK_MONOTONIC\n";
    }
    ~Presentation() {
        for (auto& [f, p] : pending_) {
            wp_presentation_feedback_destroy(f);
            delete p;
        }
        if (presentation_) wp_presentation_destroy(presentation_);
        if (registry_) wl_registry_destroy(registry_);
        if (queue_) wl_event_queue_destroy(queue_);
    }
    void Request(uint64_t pts) {
        if (pending_.size() >= 120) throw std::runtime_error("presentation feedback stalled");
        auto* f = wp_presentation_feedback(presentation_, surface_);
        auto* p = new Pending{this, pts, lat::NowUs()};
        pending_.emplace(f, p);
        static const wp_presentation_feedback_listener listener{Output, Presented, Discarded};
        wp_presentation_feedback_add_listener(f, &listener, p);
    }
    void Pump() {
        while (wl_display_prepare_read_queue(display_, queue_) != 0)
            if (wl_display_dispatch_queue_pending(display_, queue_) < 0)
                throw std::runtime_error("Wayland dispatch failed");
        wl_display_flush(display_);
        pollfd fd{wl_display_get_fd(display_), POLLIN, 0};
        if (poll(&fd, 1, 0) > 0 && (fd.revents & POLLIN)) {
            if (wl_display_read_events(display_) < 0)
                throw std::runtime_error("Wayland read failed");
        } else
            wl_display_cancel_read(display_);
        if (wl_display_dispatch_queue_pending(display_, queue_) < 0)
            throw std::runtime_error("Wayland disconnected");
    }
};
class DmaRead {
    std::set<int> fds_;

public:
    explicit DmaRead(const VideoFrameInfo& f) {
        try {
            for (unsigned p = 0; p < 2; ++p) {
                int fd = static_cast<int>(f.stVFrame.u32Fd[p]);
                if (fds_.count(fd)) continue;
                dma_buf_sync sync{DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
                if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync))
                    throw std::runtime_error("DMA read begin failed");
                fds_.insert(fd);
            }
        } catch (...) {
            End();
            throw;
        }
    }
    void End() {
        for (int fd : fds_) {
            dma_buf_sync s{DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
            ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
        }
        fds_.clear();
    }
    ~DmaRead() { End(); }
};
}  // namespace
int main(int argc, char** argv) {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    int result = 0;
    try {
        if (argc != 5 || std::string(argv[1]) != "--config" || std::string(argv[3]) != "--seconds")
            throw std::runtime_error("usage: video_transmission_ground_latency --config ground.yaml --seconds 60");
        const int seconds = std::stoi(argv[4]);
        if (seconds <= 0 || seconds > 3600) throw std::runtime_error("seconds must be 1..3600");
        auto config = video_transmission::example::LoadExampleConfig(argv[2]);
        video_transmission::example::ValidateExampleConfig(config);
        video_transmission::RequireRole(config.session, video_transmission::Role::Ground);
        std::cout << video_transmission::example::DescribeExampleConfig(config) << std::endl;
        // 独立成行：每次测量都必须能确认二进制身份，便于对照测量结果。
        std::cout << "build " << video_transmission::BuildInfo() << std::endl;
        if (!config.session.pipeline.decoded_output)
            throw std::runtime_error("decoded_callback output required");
        SdlCheck(SDL_Init(SDL_INIT_VIDEO));
        window = SDL_CreateWindow("Video transmission hardware decode latency", SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED, config.session.pipeline.width, config.session.pipeline.height,
            SDL_WINDOW_SHOWN);
        if (!window) throw std::runtime_error(SDL_GetError());
        renderer =
            SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) throw std::runtime_error(SDL_GetError());
        SDL_RendererInfo renderer_info{};
        SDL_GetRendererInfo(renderer, &renderer_info);
        std::cerr << "SDL renderer=" << renderer_info.name << '\n';
        SdlCheck(SDL_RenderClear(renderer));
        SDL_RenderPresent(renderer);
        Presentation presentation(window);
        std::mutex mutex;
        video_transmission::MppFrame latest;
        // Session is destroyed before the callback's mailbox/mutex and renderer.
        video_transmission::VideoTransmission session;
        session.OnState([](const auto& r) {
            std::cerr << "generation=" << r.generation << ' ' << r.message << '\n';
        });
        session.OnVideoFrame([&](video_transmission::MppFrame f) {
            if (!f.owner) return;
            std::lock_guard lock(mutex);
            if (latest.owner) lat::Mark("gui_mailbox_drop", latest.info.stVFrame.u64PTS);
            latest = std::move(f);
        });
        std::signal(SIGINT, Exit);
        std::signal(SIGTERM, Exit);
        auto started = session.Start(config.session);
        if (!started.ok()) throw std::runtime_error(started.message);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        unsigned width = 0, height = 0;
        while (!exiting && std::chrono::steady_clock::now() < until) {
            SDL_Event event;
            while (SDL_PollEvent(&event))
                if (event.type == SDL_QUIT) exiting = 1;
            presentation.Pump();
            video_transmission::MppFrame f;
            {
                std::lock_guard lock(mutex);
                f = std::move(latest);
                latest = {};
            }
            if (!f.owner) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            // 整帧租约随 mailbox 转移；完成纹理复制和 DMA 同步后再释放 owner。
            const auto& v = f.info;
            const auto pts = v.stVFrame.u64PTS;
            lat::Mark("gui_begin", pts);
            if (v.stCommFrameInfo.ePixelFormat != MPP_PIXEL_FORMAT_NV12)
                throw std::runtime_error("expected linear NV12 decoder output");
            if (width != v.stCommFrameInfo.u32Width || height != v.stCommFrameInfo.u32Height) {
                width = v.stCommFrameInfo.u32Width;
                height = v.stCommFrameInfo.u32Height;
                if (texture) SDL_DestroyTexture(texture);
                texture = SDL_CreateTexture(
                    renderer, SDL_PIXELFORMAT_NV12, SDL_TEXTUREACCESS_STREAMING, width, height);
                if (!texture) throw std::runtime_error(SDL_GetError());
            }
            {
                DmaRead read(v);
                SdlCheck(SDL_UpdateNVTexture(texture, nullptr,
                    reinterpret_cast<const uint8_t*>(v.stVFrame.ulPlaneVirAddr[0]),
                    v.stVFrame.u32PlaneStride[0],
                    reinterpret_cast<const uint8_t*>(v.stVFrame.ulPlaneVirAddr[1]),
                    v.stVFrame.u32PlaneStride[1]));
            }
            f = {};
            SdlCheck(SDL_RenderClear(renderer));
            SdlCheck(SDL_RenderCopy(renderer, texture, nullptr, nullptr));
            presentation.Request(pts);
            lat::Mark("gui_submit", pts);
            SDL_RenderPresent(renderer);
            lat::Mark("gui_return", pts);
        }
        {
            std::lock_guard lock(mutex);
            latest = {};
        }
        session.Stop();
        {
            std::lock_guard lock(mutex);
            latest = {};
        }
        // Drain final feedback without leaving the decoder running.
        for (int i = 0; i < 50; ++i) {
            SDL_PumpEvents();
            presentation.Pump();
            SDL_Delay(2);
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
