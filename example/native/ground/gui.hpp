#ifndef GUI_HPP
#define GUI_HPP

#include <SDL.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>

#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <video_transmission/video_transmission.hpp>
#include <video_transmission/video_transmission.hpp>

#include "pts_playout.hpp"

class GroundGui {
    struct Surface {
        SDL_Texture* texture = nullptr;
        ~Surface() { SDL_DestroyTexture(texture); }
    };
    using Queue = video_transmission_example::PtsPlayout<std::shared_ptr<Surface>>;
    using Clock = Queue::Clock;
    struct Pending {
        video_transmission::MppFrame frame;
        Clock::time_point arrived;
    };

public:
    explicit GroundGui(video_transmission_example::PlayoutConfig config = {}, bool fullscreen = false)
        : config_(config), queue_(config) {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) throw std::runtime_error(SDL_GetError());
        window_ =
            SDL_CreateWindow("Video Transmission Ground", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 600,
                SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
                    (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
        if (window_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_ && window_)
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer_) {
            const std::string error = SDL_GetError();
            Cleanup();
            throw std::runtime_error(error);
        }
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        SDL_RenderPresent(renderer_);
        SDL_RendererInfo info{};
        SDL_GetRendererInfo(renderer_, &info);
        if (fullscreen) SDL_ShowCursor(SDL_DISABLE);
        int window_width = 0, window_height = 0;
        SDL_GetWindowSize(window_, &window_width, &window_height);
        std::cout << "GUI PTS playout: delay=" << config_.delay_ms
                    << "ms max_local_age=" << config_.max_age_ms
                    << "ms max_frames=" << config_.max_frames
                    << " renderer=" << (info.name ? info.name : "unknown")
                    << " window=" << window_width << 'x' << window_height
                    << " fullscreen=" << ((SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0)
                    << '\n';
    }
    ~GroundGui() { Cleanup(); }
    GroundGui(const GroundGui&) = delete;
    GroundGui& operator=(const GroundGui&) = delete;

    void Push(video_transmission::MppFrame frame) {
        if (!frame.owner) return;
        const auto now = Clock::now();
        bool wake;
        {
            std::lock_guard lock(mutex_);
            ++received_;
            wake = pending_.empty();
            // Only the upload mailbox holds MPP leases, never the PTS queue.
            const size_t capacity = config_.delay_ms ? 4 : 1;
            while (pending_.size() >= capacity) {
                pending_.pop_front();
                ++pending_dropped_;
            }
            pending_.push_back({std::move(frame), now});
        }
        if (wake) {
            SDL_Event event{};
            event.type = SDL_USEREVENT;
            SDL_PushEvent(&event);
        }
    }

    bool Poll() {
        int wait_ms = queue_.WaitMs(Clock::now());
        {
            std::lock_guard lock(mutex_);
            if (!pending_.empty()) wait_ms = 0;
        }
        SDL_Event event{};
        if (SDL_WaitEventTimeout(&event, wait_ms)) {
            do {
                if (event.type == SDL_QUIT ||
                    (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE))
                    return false;
                if (event.type == SDL_WINDOWEVENT &&
                    (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                        event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED))
                    redraw_ = true;
            } while (SDL_PollEvent(&event));
        }
        Pending input{};
        {
            std::lock_guard lock(mutex_);
            if (!pending_.empty()) {
                input = std::move(pending_.front());
                pending_.pop_front();
            }
        }
        if (input.frame.owner) Upload(std::move(input));
        if (auto item = queue_.Pop(Clock::now())) {
            displayed_ = std::move(item->payload);
            const auto due =
                std::chrono::duration_cast<std::chrono::microseconds>(item->due.time_since_epoch())
                    .count();
            video_transmission::latency::Mark("gui_due", item->pts_us, due);
            video_transmission::latency::Mark("gui_submit", item->pts_us);
            Draw();
            video_transmission::latency::Mark("gui_return", item->pts_us);
            ++presented_;
        } else if (redraw_ && displayed_) {
            Draw();
        }
        PrintStats();
        return true;
    }

private:
    static void Check(int result) {
        if (result < 0) throw std::runtime_error(SDL_GetError());
    }

    void Draw() {
        Check(SDL_RenderClear(renderer_));
        Check(SDL_RenderCopy(renderer_, displayed_->texture, nullptr, nullptr));
        SDL_RenderPresent(renderer_);
        redraw_ = false;
    }

    std::shared_ptr<Surface> AcquireSurface(int width, int height) {
        if (width != width_ || height != height_) {
            queue_.Clear();
            displayed_.reset();
            surfaces_.clear();
            width_ = width;
            height_ = height;
            Check(SDL_RenderSetLogicalSize(renderer_, width, height));
        }
        for (auto& surface : surfaces_) {
            if (surface.use_count() == 1) return surface;
        }
        // Cap both surface count and nominal NV12 bytes (driver overhead is extra).
        constexpr uint64_t kTextureBytes = 64 * 1024 * 1024;
        const uint64_t bytes = uint64_t(width) * height * 3 / 2;
        if (surfaces_.size() >= config_.max_frames + 2 ||
            bytes > kTextureBytes / (surfaces_.size() + 1)) {
            ++texture_dropped_;
            return {};
        }
        auto surface = std::make_shared<Surface>();
        surface->texture = SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_NV12, SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!surface->texture) throw std::runtime_error(SDL_GetError());
        surfaces_.push_back(surface);
        return surface;
    }

    void Upload(Pending input) {
        auto& frame = input.frame;
        const auto& info = frame.info;
        const auto& v = info.stVFrame;
        const int width = info.stCommFrameInfo.u32Width;
        const int height = info.stCommFrameInfo.u32Height;
        const auto pts = v.u64PTS;
        if (info.stCommFrameInfo.ePixelFormat != MPP_PIXEL_FORMAT_NV12 || width <= 0 ||
            height <= 0 || !v.ulPlaneVirAddr[0] || !v.ulPlaneVirAddr[1])
            throw std::runtime_error("GUI requires mapped linear NV12 output");
        if (have_generation_ && frame.generation != generation_) queue_.Clear();
        generation_ = frame.generation;
        have_generation_ = true;
        if (Clock::now() - input.arrived > std::chrono::milliseconds(config_.max_age_ms)) {
            ++upload_expired_;
            return;
        }
        auto surface = AcquireSurface(width, height);
        if (!surface) return;
        video_transmission::latency::Mark("gui_upload_begin", pts);
        // Synchronize CPU reads with decoder DMA writes, once per unique fd.
        const int fds[] = {static_cast<int>(v.u32Fd[0]), static_cast<int>(v.u32Fd[1])};
        int started = 0;
        auto end = [&] {
            for (int p = 0; p < started; ++p) {
                if (p == 1 && fds[1] == fds[0]) continue;
                dma_buf_sync sync{DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
                ioctl(fds[p], DMA_BUF_IOCTL_SYNC, &sync);
            }
        };
        try {
            for (int p = 0; p < 2; ++p) {
                if (p == 1 && fds[1] == fds[0]) {
                    ++started;
                    continue;
                }
                dma_buf_sync sync{DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
                if (ioctl(fds[p], DMA_BUF_IOCTL_SYNC, &sync) < 0)
                    throw std::runtime_error("DMA read synchronization failed");
                ++started;
            }
            Check(SDL_UpdateNVTexture(surface->texture, nullptr,
                reinterpret_cast<const uint8_t*>(v.ulPlaneVirAddr[0]), v.u32PlaneStride[0],
                reinterpret_cast<const uint8_t*>(v.ulPlaneVirAddr[1]), v.u32PlaneStride[1]));
        } catch (...) {
            end();
            throw;
        }
        end();
        // Return the decoder lease before waiting for the configured presentation time.
        frame = {};
        video_transmission::latency::Mark("gui_queued", pts);
        queue_.Push(std::move(surface), pts, input.arrived, Clock::now());
        ++uploaded_;
    }

    void PrintStats() {
        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - stats_at_).count();
        if (elapsed < 5) return;
        uint64_t received, dropped;
        size_t pending;
        {
            std::lock_guard lock(mutex_);
            received = received_;
            dropped = pending_dropped_;
            pending = pending_.size();
        }
        const auto& s = queue_.Stats();
        std::cout << "GUI playout: received=" << received << " uploaded=" << uploaded_
                    << " presented=" << presented_
                    << " fps=" << (presented_ - last_presented_) / elapsed
                    << " queued=" << queue_.Size() << " pending=" << pending
                    << " textures=" << surfaces_.size() << " pending_drop=" << dropped
                    << " texture_drop=" << texture_dropped_ << " upload_expired=" << upload_expired_
                    << " overflow=" << s.overflow << " expired=" << s.expired << " late=" << s.late
                    << " reset=" << s.reset << " invalid=" << s.invalid
                    << " clock_advance=" << s.clock_advance << '\n';
        last_presented_ = presented_;
        stats_at_ = now;
    }

    void Cleanup() {
        pending_.clear();
        queue_.Clear();
        displayed_.reset();
        surfaces_.clear();
        SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
        SDL_Quit();
    }
    video_transmission_example::PlayoutConfig config_;
    Queue queue_;
    std::mutex mutex_;
    std::deque<Pending> pending_;
    std::vector<std::shared_ptr<Surface>> surfaces_;
    std::shared_ptr<Surface> displayed_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    int width_ = 0, height_ = 0;
    bool redraw_ = false, have_generation_ = false;
    uint64_t generation_ = 0;
    uint64_t received_ = 0, pending_dropped_ = 0;
    uint64_t uploaded_ = 0, presented_ = 0, last_presented_ = 0;
    uint64_t texture_dropped_ = 0, upload_expired_ = 0;
    Clock::time_point stats_at_ = Clock::now();
};

#endif  // GUI_HPP
