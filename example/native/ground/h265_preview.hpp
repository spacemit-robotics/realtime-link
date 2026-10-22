#ifndef H265_PREVIEW_HPP
#define H265_PREVIEW_HPP

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <video_transmission/video_transmission.hpp>

// Bounded asynchronous bridge from the encoded callback to a local HEVC player.
class H265Preview {
public:
    explicit H265Preview(std::string path) : path_(std::move(path)) {
        struct stat st{};
        if (stat(path_.c_str(), &st) != 0 || !S_ISFIFO(st.st_mode))
            throw std::runtime_error("preview path must be an existing FIFO: " + path_);
        worker_ = std::thread([this] { Run(); });
    }

    ~H265Preview() {
        stop_ = true;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    H265Preview(const H265Preview&) = delete;
    H265Preview& operator=(const H265Preview&) = delete;

    void Push(video_transmission::EncodedFrame frame) {
        if (!frame.data || !frame.size || frame.size > kMaxBytes) return;
        std::lock_guard<std::mutex> lock(mu_);
        if (!connected_) return;
        if (frames_.size() >= kMaxFrames || bytes_ + frame.size > kMaxBytes) {
            frames_.clear();
            bytes_ = 0;
            need_keyframe_ = true;
        }
        if (need_keyframe_) {
            if (!frame.keyframe) return;
            need_keyframe_ = false;
        }
        bytes_ += frame.size;
        frames_.push_back(std::move(frame));
        cv_.notify_one();
    }

private:
    static constexpr size_t kMaxFrames = 60;
    static constexpr size_t kMaxBytes = 4 * 1024 * 1024;

    void Reset() {
        std::lock_guard<std::mutex> lock(mu_);
        frames_.clear();
        bytes_ = 0;
        connected_ = false;
        need_keyframe_ = true;
    }

    void Run() {
        int fd = -1;
        while (!stop_) {
            if (fd < 0) {
                fd = open(path_.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
                if (fd < 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                std::lock_guard<std::mutex> lock(mu_);
                connected_ = true;
                need_keyframe_ = true;
            }

            video_transmission::EncodedFrame frame;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait_for(lock, std::chrono::milliseconds(100),
                    [this] { return stop_ || !frames_.empty(); });
                if (stop_) break;
                if (frames_.empty()) continue;
                frame = std::move(frames_.front());
                frames_.pop_front();
                bytes_ -= frame.size;
            }

            size_t offset = 0;
            bool failed = false;
            while (offset < frame.size && !stop_) {
                const ssize_t n = write(fd, frame.data.get() + offset, frame.size - offset);
                if (n > 0) {
                    offset += static_cast<size_t>(n);
                } else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                    pollfd pfd{fd, POLLOUT, 0};
                    if (poll(&pfd, 1, 100) < 0 && errno != EINTR) {
                        failed = true;
                        break;
                    }
                } else {
                    failed = true;
                    break;
                }
            }
            if (failed) {
                close(fd);
                fd = -1;
                Reset();
            }
        }
        if (fd >= 0) close(fd);
    }

    std::string path_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<video_transmission::EncodedFrame> frames_;
    size_t bytes_ = 0;
    bool connected_ = false;
    bool need_keyframe_ = true;
};

#endif  // H265_PREVIEW_HPP
