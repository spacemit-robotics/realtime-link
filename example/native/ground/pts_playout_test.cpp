#include "pts_playout.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

using namespace std::chrono_literals;  // NOLINT(build/namespaces)
using Queue = video_transmission_example::PtsPlayout<std::shared_ptr<int>>;
using Time = Queue::Time;

void Check(bool value) {
    if (!value) throw std::runtime_error("PTS playout regression failed");
}
auto Frame(int n) { return std::make_shared<int>(n); }

int main() {
    const Time start{};
    Queue queue({300, 600, 16});
    auto held = Frame(0);
    queue.Push(held, 1000000, start, start);
    Check(held.use_count() == 2);
    Check(!queue.Pop(start + 299ms));
    Check(queue.WaitMs(start + 299ms) == 1);
    auto first = queue.Pop(start + 300ms);
    Check(first && *first->payload == 0);
    first.reset();
    Check(held.use_count() == 1);
    queue.Push(Frame(1), 1033333, start + 220ms, start + 220ms);
    queue.Push(Frame(2), 1066666, start + 224ms, start + 224ms);
    Check(!queue.Pop(start + 333ms));
    Check(*queue.Pop(start + 333333us)->payload == 1);
    Check(*queue.Pop(start + 366666us)->payload == 2);

    // Periodic 200 ms I-frame stalls, followed by fast catch-up P frames.
    for (const auto config : {video_transmission_example::PlayoutConfig{250, 400, 16}, {300, 600, 16}}) {
        Queue periodic(config);
        struct Arrival {
            uint64_t pts;
            int64_t at;
        };
        std::vector<Arrival> arrivals;
        int64_t previous = -10000;
        for (int i = 0; i < 300; ++i) {
            const int64_t pts = i * 33333LL;
            const int64_t transfer = i % 30 == 0 ? 200000 : 4000;
            previous = std::max(previous, pts) + transfer;
            arrivals.push_back({uint64_t(1000000 + pts), previous});
        }
        size_t input = 0;
        int output = 0;
        Time last{};
        int64_t worst_error = 0;
        for (int64_t us = 0; us < 11000000; us += 1000) {
            while (input < arrivals.size() && arrivals[input].at <= us) {
                periodic.Push(Frame(input), arrivals[input].pts,
                    start + std::chrono::microseconds(arrivals[input].at),
                    start + std::chrono::microseconds(us));
                ++input;
            }
            if (auto out = periodic.Pop(start + std::chrono::microseconds(us))) {
                Check(*out->payload == output);
                if (output) {
                    const auto interval = std::chrono::duration_cast<std::chrono::microseconds>(
                        start + std::chrono::microseconds(us) - last)
                                                .count();
                    worst_error = std::max<int64_t>(worst_error, std::abs(interval - 33333));
                }
                last = start + std::chrono::microseconds(us);
                ++output;
            }
        }
        Check(output == 300 && worst_error <= 1000);
        Check(periodic.Stats().late == 0 && periodic.Stats().overflow == 0);
    }

    // A decoder's first frame can be 350ms behind the subsequent steady stream.
    // Do not anchor playback permanently to that startup backlog.
    Queue startup({300, 600, 16});
    startup.Push(Frame(0), 1000000, start + 350ms, start + 350ms);
    startup.Push(Frame(1), 1033333, start + 352ms, start + 352ms);
    startup.Push(Frame(2), 1066666, start + 354ms, start + 354ms);
    startup.Push(Frame(3), 1366666, start + 370ms, start + 370ms);
    auto ready = startup.Pop(start + 370ms);
    Check(ready && *ready->payload == 2);
    Check(startup.Stats().clock_advance == 3 && startup.Stats().late == 2);
    Check(!startup.Pop(start + 669ms));
    Check(*startup.Pop(start + 670ms)->payload == 3);

    Queue bounded({300, 600, 2});
    auto one = Frame(1);
    bounded.Push(one, 1000000, start, start);
    bounded.Push(Frame(2), 1033333, start, start);
    bounded.Push(Frame(3), 1066666, start, start);
    Check(bounded.Size() == 2 && bounded.Stats().overflow == 1 && one.use_count() == 1);
    Check(!bounded.Pop(start + 601ms));
    Check(bounded.Stats().expired == 2);
    bounded.Push(Frame(4), 1100000, start + 700ms, start + 700ms);
    Check(!bounded.Pop(start + 999ms));
    Check(*bounded.Pop(start + 1000ms)->payload == 4);
    bounded.Push(Frame(5), 1, start + 1100ms, start + 1100ms);
    Check(bounded.Stats().reset == 1);
    Check(!bounded.Pop(start + 1399ms));
    Check(*bounded.Pop(start + 1400ms)->payload == 5);
    bounded.Push(Frame(6), 20000000, start + 1500ms, start + 1500ms);
    Check(bounded.Stats().reset == 2);

    Queue late({300, 600, 16});
    late.Push(Frame(1), 1000000, start, start);
    late.Push(Frame(2), 1033333, start, start);
    Check(*late.Pop(start + 340ms)->payload == 2 && late.Stats().late == 1);
    late.Clear();
    Check(late.Size() == 0);
    Queue immediate({0, 600, 16});
    immediate.Push(Frame(1), 1000000, start, start);
    immediate.Push(Frame(2), 1033333, start, start);
    Check(*immediate.Pop(start)->payload == 2);
    immediate.Push(Frame(3), 0, start, start);
    Check(immediate.Stats().invalid == 1);
    // The deployed compromise is 250ms/400ms; no wall-clock sleeps in tests.
    Queue deployed({250, 400, 16});
    auto retained = Frame(7);
    deployed.Push(retained, 1000000, start, start);
    Check(!deployed.Pop(start + 249ms));
    Check(deployed.WaitMs(start + 249ms) == 1);
    auto displayed = deployed.Pop(start + 250ms);
    Check(displayed && *displayed->payload == 7);
    displayed.reset();
    Check(retained.use_count() == 1);
    deployed.Push(retained, 1033333, start + 30ms, start + 30ms);
    deployed.Clear();
    Check(deployed.Size() == 0 && retained.use_count() == 1);
    Check(deployed.WaitMs(start + 30ms) == 50);

    // Duplicate/non-monotonic PTS resets the presentation epoch, not the decoder.
    deployed.Push(Frame(8), 1000000, start + 40ms, start + 40ms);
    deployed.Push(Frame(9), 1000000, start + 50ms, start + 50ms);
    Check(deployed.Stats().reset == 1 && deployed.Size() == 1);
    Check(!deployed.Pop(start + 299ms));
    Check(*deployed.Pop(start + 300ms)->payload == 9);
    auto stale = Frame(10);
    deployed.Push(stale, 1033333, start, start + 401ms);
    Check(deployed.Stats().invalid == 1 && stale.use_count() == 1);
    Check(deployed.Size() == 0);

    // PTS is relative to a stream epoch, not a ground-host wall clock.
    Queue high_pts({250, 400, 16});
    const uint64_t base = UINT64_MAX - 1000000;
    high_pts.Push(Frame(1), base, start, start);
    high_pts.Push(Frame(2), base + 33333, start + 34ms, start + 34ms);
    Check(*high_pts.Pop(start + 250ms)->payload == 1);
    Check(*high_pts.Pop(start + 284ms)->payload == 2);

    for (const auto config :
        {video_transmission_example::PlayoutConfig{0, 0, 16}, {250, 400, 0}, {250, 400, 121}, {0, 10001, 16}}) {
        bool invalid = false;
        try {
            Queue bad(config);
        } catch (const std::invalid_argument&) {
            invalid = true;
        }
        Check(invalid);
    }
    bool rejected = false;
    try {
        Queue invalid({600, 600, 16});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    Check(rejected);
    std::cout << "PTS playout: 300/300 frames at both 250ms and 300ms, 200ms bursts smoothed; "
                "bounds, reset, expiry, late/drop, ownership and zero-delay: PASS\n";
}
