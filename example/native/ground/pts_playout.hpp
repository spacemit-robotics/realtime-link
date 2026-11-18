#ifndef PTS_PLAYOUT_HPP
#define PTS_PLAYOUT_HPP

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <utility>

namespace video_transmission_example {

struct PlayoutConfig {
    uint32_t delay_ms = 0;
    uint32_t max_age_ms = 600;
    size_t max_frames = 16;
};

// Single-threaded presentation queue. Payloads must be independent of decoder
// buffers (the GUI uses GPU textures). PTS deltas, not host clock offsets, set
// deadlines. Encoded reference frames must never be dropped by this queue.
template <class T>
class PtsPlayout {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    struct Item {
        T payload;
        uint64_t pts_us;
        Time arrived;
        Time due;
    };
    struct Counters {
        uint64_t overflow = 0, expired = 0, late = 0, reset = 0, invalid = 0;
        uint64_t clock_advance = 0;
    };

    explicit PtsPlayout(PlayoutConfig config) : config_(config) {
        if (config.max_age_ms == 0 || config.max_age_ms > 10000 ||
            config.delay_ms >= config.max_age_ms || config.max_frames == 0 ||
            config.max_frames > 120)
            throw std::invalid_argument("invalid bounded PTS playout configuration");
    }

    void Clear() {
        queue_.clear();
        anchored_ = false;
    }

    void Push(T payload, uint64_t pts_us, Time arrived, Time now) {
        Prune(now);
        if (pts_us == 0 || now - arrived > std::chrono::milliseconds(config_.max_age_ms)) {
            ++counters_.invalid;
            return;
        }
        if (anchored_ &&
            (pts_us <= last_pts_ || pts_us - last_pts_ > uint64_t(config_.max_age_ms) * 1000)) {
            Clear();
            ++counters_.reset;
        }
        if (!anchored_) {
            base_pts_ = pts_us;
            base_time_ = arrived + std::chrono::milliseconds(config_.delay_ms);
            anchored_ = true;
        }
        // The first I frame may arrive after decoder startup/backlog. Anchor to
        // the minimum observed transit, not permanently to that delayed frame.
        // Only advance the clock; network stalls never push deadlines later.
        const auto offset = std::chrono::microseconds(pts_us - base_pts_);
        const auto candidate = arrived + std::chrono::milliseconds(config_.delay_ms) - offset;
        if (config_.delay_ms && candidate < base_time_) {
            const auto advance = base_time_ - candidate;
            base_time_ = candidate;
            for (auto& item : queue_) item.due -= advance;
            ++counters_.clock_advance;
        }
        last_pts_ = pts_us;
        if (config_.delay_ms == 0) {
            counters_.overflow += queue_.size();
            queue_.clear();
        }
        while (queue_.size() >= config_.max_frames) {
            queue_.pop_front();
            ++counters_.overflow;
        }
        const auto due = config_.delay_ms ? base_time_ + offset : arrived;
        queue_.push_back({std::move(payload), pts_us, arrived, due});
    }

    std::optional<Item> Pop(Time now) {
        Prune(now);
        if (queue_.empty() || queue_.front().due > now) return {};
        // A stopped/minimized window must not replay a stale burst at full speed.
        // This only sheds already decoded display frames after a missed deadline.
        while (queue_.size() > 1 && queue_[1].due <= now) {
            queue_.pop_front();
            ++counters_.late;
        }
        auto item = std::move(queue_.front());
        queue_.pop_front();
        return item;
    }

    int WaitMs(Time now) const {
        if (queue_.empty()) return 50;
        if (queue_.front().due <= now) return 0;
        const auto us =
            std::chrono::duration_cast<std::chrono::microseconds>(queue_.front().due - now).count();
        return static_cast<int>(std::min<int64_t>(50, (us + 999) / 1000));
    }
    size_t Size() const { return queue_.size(); }
    const Counters& Stats() const { return counters_; }

private:
    void Prune(Time now) {
        bool expired = false;
        while (!queue_.empty() &&
            now - queue_.front().arrived > std::chrono::milliseconds(config_.max_age_ms)) {
            queue_.pop_front();
            ++counters_.expired;
            expired = true;
        }
        if (expired && queue_.empty()) anchored_ = false;
    }
    PlayoutConfig config_;
    Counters counters_;
    std::deque<Item> queue_;
    bool anchored_ = false;
    uint64_t base_pts_ = 0, last_pts_ = 0;
    Time base_time_{};
};
}  // namespace video_transmission_example

#endif  // PTS_PLAYOUT_HPP
