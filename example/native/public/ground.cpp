#include "common.hpp"
#if VT_PUBLIC_GUI
#include <SDL.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#endif
#include <memory>
#include <mutex>
#include <vector>
#include <cstring>
#include <sys/sys_api.h>
#include <sys/vb_api.h>
#include <vdec/vdec_api.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
namespace {
#if VT_PUBLIC_GUI
struct Preview {
    std::mutex mutex;
    std::vector<unsigned char> pixels;
    unsigned width = 0, height = 0;
    unsigned long received = 0, displayed = 0;
    std::atomic<unsigned long> failed{0};
    bool pending = false;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    unsigned tw = 0, th = 0;
    Preview() {
        if (SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
        window = SDL_CreateWindow("Video Transmission Ground", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 600,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (window) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (window && !renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer) {
            std::string e = SDL_GetError();
            SDL_DestroyWindow(window);
            SDL_Quit();
            throw std::runtime_error(e);
        }
        SDL_RendererInfo info{};
        SDL_GetRendererInfo(renderer, &info);
        std::printf("GUI renderer=%s driver=%s\n", info.name, SDL_GetCurrentVideoDriver());
    }
    ~Preview() {
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
    void push(const vt_raw_frame* f) noexcept {
        try {
            auto i = vt_mpp_frame_info(f);
            if (!i || f->pixel_format != MPP_PIXEL_FORMAT_NV12 || !f->width || !f->height || (f->width & 1) ||
                (f->height & 1)) {
                ++failed;
                return;
            }
            const auto& v = i->stVFrame;
            if (!v.ulPlaneVirAddr[0] || !v.ulPlaneVirAddr[1] || v.u32PlaneStride[0] < f->width ||
                v.u32PlaneStride[1] < f->width) {
                ++failed;
                return;
            }
            std::lock_guard<std::mutex> lock(mutex);
            pending = false;
            pixels.resize(size_t(f->width) * f->height * 3 / 2);
            width = f->width;
            height = f->height;
            int fd0 = static_cast<int>(v.u32Fd[0]), fd1 = static_cast<int>(v.u32Fd[1]);
            dma_buf_sync start{DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ}, end{DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
            if (ioctl(fd0, DMA_BUF_IOCTL_SYNC, &start) < 0) {
                ++failed;
                return;
            }
            if (fd1 != fd0 && ioctl(fd1, DMA_BUF_IOCTL_SYNC, &start) < 0) {
                ioctl(fd0, DMA_BUF_IOCTL_SYNC, &end);
                ++failed;
                return;
            }
            {
                for (unsigned row = 0; row < height; row++)
                    std::memcpy(pixels.data() + size_t(row) * width,
                        reinterpret_cast<const unsigned char*>(v.ulPlaneVirAddr[0]) + size_t(row) * v.u32PlaneStride[0],
                        width);
                for (unsigned row = 0; row < height / 2; row++)
                    std::memcpy(pixels.data() + size_t(width) * height + size_t(row) * width,
                        reinterpret_cast<const unsigned char*>(v.ulPlaneVirAddr[1]) + size_t(row) * v.u32PlaneStride[1],
                        width);
                pending = true;
                ++received;
            }
            if (fd1 != fd0) ioctl(fd1, DMA_BUF_IOCTL_SYNC, &end);
            ioctl(fd0, DMA_BUF_IOCTL_SYNC, &end);
        } catch (...) {
            ++failed;
        }
    }
    bool poll() {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            if (event.type == SDL_QUIT || (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE))
                return false;
        std::lock_guard<std::mutex> lock(mutex);
        if (pending) {
            if (!texture || tw != width || th != height) {
                SDL_DestroyTexture(texture);
                texture = nullptr;
                texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_NV12, SDL_TEXTUREACCESS_STREAMING, width, height);
                if (!texture) throw std::runtime_error(SDL_GetError());
                tw = width;
                th = height;
                SDL_RenderSetLogicalSize(renderer, width, height);
            }
            if (SDL_UpdateNVTexture(
                    texture, nullptr, pixels.data(), width, pixels.data() + size_t(width) * height, width) < 0 ||
                SDL_RenderClear(renderer) < 0 || SDL_RenderCopy(renderer, texture, nullptr, nullptr) < 0)
                throw std::runtime_error(SDL_GetError());
            SDL_RenderPresent(renderer);
            pending = false;
            ++displayed;
            if (displayed <= 3 || displayed % 300 == 0) {
                std::printf("gui presented=%lu received=%lu failed=%lu\n", displayed, received, failed.load());
                std::fflush(stdout);
            }
        }
        return true;
    }
};
#else
struct Preview {
    Preview() { throw std::runtime_error("GUI support unavailable; install SDL2 and rebuild"); }
    void push(const vt_raw_frame*) noexcept {}
    bool poll() { return false; }
    unsigned long displayed = 0, received = 0;
    std::atomic<unsigned long> failed{0};
};
#endif
Preview* preview = nullptr;
volatile std::sig_atomic_t quit = 0;
void sig(int) { quit = 1; }
void on_raw(void* p, const vt_raw_frame* f) {
    auto& e = *static_cast<NativeEvents*>(p);
    if (preview) preview->push(f);
    auto n = ++e.frames;
    if (n <= 3 || n % 120 == 0) {
        std::printf("ground decoded=%lu %ux%u pts=%llu\n", n, f->width, f->height, (unsigned long long)f->pts_us);
        std::fflush(stdout);
    }
}
}  // namespace
int main(int argc, char** argv) {
    if (argc < 6 || argc > 8) {
        std::fprintf(stderr, "usage: %s PLUGIN CONFIG SECONDS WIDTH HEIGHT [--request-idr] [--gui]\n", argv[0]);
        return 2;
    }
    bool manual_request = false, use_gui = false;
    for (int i = 6; i < argc; i++) {
        if (std::string(argv[i]) == "--request-idr")
            manual_request = true;
        else if (std::string(argv[i]) == "--gui")
            use_gui = true;
        else
            return 2;
    }
    uint32_t seconds, w, h;
    try {
        seconds = native_number(argv[3]);
        w = native_number(argv[4]);
        h = native_number(argv[5]);
    } catch (...) {
        return 2;
    }
    if (SYS_Init() || VB_Init() || VDEC_Init()) {
        std::fprintf(stderr, "MPP init failed\n");
        return 1;
    }
    int result = 1;
    try {
        std::unique_ptr<Preview> gui;
        if (use_gui) {
            gui = std::make_unique<Preview>();
            preview = gui.get();
        }
        NativePlugin plugin(argv[1]);
        NativeEvents events;
        NativeSession session;
        std::signal(SIGINT, sig);
        std::signal(SIGTERM, sig);
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        unsigned long epoch = 0;
        bool requested = false;
        NativeRequestRetry retry;
        while (!quit && std::chrono::steady_clock::now() < end) {
            if (!session.get() || events.reconnect.exchange(false)) {
                session.reset();
                auto rc = session.open(plugin, argv[2], VT_GROUND, events, w, h, 1, 1, 1, on_raw);
                if (rc != VT_OK) {
                    std::fprintf(stderr, "session start status=%d\n", rc);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
                retry = {};
                std::printf("ground epoch=%lu\n", ++epoch);
                std::fflush(stdout);
            }
            if (manual_request && !requested && events.frames.load()) {
                events.keyframe = true;
                requested = true;
            }
            if (events.keyframe.exchange(false)) retry.signal();
            const auto now = NativeRequestRetry::Clock::now();
            if (retry.ready(now)) {
                auto rc = request_keyframe(session);
                retry.finish(rc, now);
                std::printf("keyframe_request status=%d\n", rc);
                std::fflush(stdout);
            }
            if (gui && !gui->poll()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        session.reset();
        if (gui) {
            std::printf(
                "gui final presented=%lu received=%lu failed=%lu\n", gui->displayed, gui->received, gui->failed.load());
            preview = nullptr;
        }
        std::printf("ground frames=%lu epochs=%lu\n", events.frames.load(), epoch);
        result = events.frames ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
    }
    VDEC_Exit();
    VB_Exit();
    SYS_Exit();
    return result;
}
