#include "usb.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
namespace video_transmission::detail {
namespace {
void Put(uint8_t* p, uint32_t x) {
    for (int i = 0; i < 4; ++i) p[i] = x >> (i * 8);
}
uint32_t Get(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void Header(uint8_t* p, uint32_t type, uint32_t len) {
    Put(p, 0xfafafafa);
    Put(p + 4, 0x01000000);
    Put(p + 8, type);
    Put(p + 12, len);
}
std::array<uint8_t, 1024> Notice(uint32_t channel, uint32_t len) {
    std::array<uint8_t, 1024> a;
    a.fill(255);
    std::memset(a.data(), 0, 52);
    Header(a.data(), 0x20, 0x40);
    Put(a.data() + 32, 0x5011);
    Put(a.data() + 36, 12);
    Put(a.data() + 40, channel);
    Put(a.data() + 44, len);
    return a;
}
}  // namespace
UsbLink::~UsbLink() {
    Cancel();
    if (control_thread_.joinable()) control_thread_.join();
    for (int f : {video_, control_in_, control_out_, interrupt_})
        if (f >= 0) close(f);
}
void UsbLink::Cancel() { running_ = false; }
bool UsbLink::Transfer(int fd, void* ptr, size_t n, bool writing, int timeout) {
    const size_t original = n;
    auto* p = static_cast<uint8_t*>(ptr);
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    while (n && running_ && std::chrono::steady_clock::now() < end) {
        pollfd d{fd, short(writing ? POLLOUT : POLLIN), 0};
        int r = poll(&d, 1, 20);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 || (d.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
        if (!r) continue;
        ssize_t done = writing ? write(fd, p, n) : read(fd, p, n);
        if (done < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (done <= 0) return false;
        p += done;
        n -= done;
    }
    if (n && n != original) running_ = false;
    return n == 0;
}
bool UsbLink::Open(const SessionConfig& c, bool sky) {
    limits_ = c.limits;
    auto ep = [&](int n) {
        return open(
            (c.ffs_root + "/ep" + std::to_string(n)).c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    };
    video_ = ep(sky ? 1 : 2);
    control_in_ = ep(3);
    control_out_ = ep(4);
    interrupt_ = ep(7);
    if (video_ < 0 || control_in_ < 0 || control_out_ < 0 || interrupt_ < 0) return false;
    running_ = true;
    if (!Command(0xa00e)) {
        running_ = false;
        return false;
    }
    control_thread_ = std::thread([this] { Controls(); });
    return true;
}
bool UsbLink::Command(uint32_t tag) {
    std::lock_guard<std::mutex> l(write_mutex_);
    auto notice = Notice(1, 40);
    std::array<uint8_t, 40> p{};
    Header(p.data(), 0x12, p.size());
    Put(p.data() + 32, tag);
    return Transfer(interrupt_, notice.data(), notice.size(), true, 200) &&
        Transfer(control_in_, p.data(), p.size(), true, 200);
}
void UsbLink::Controls() {
    while (running_) {
        pollfd p{control_out_, POLLIN, 0};
        int rc = poll(&p, 1, 50);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            running_ = false;
            break;
        }
        if (!rc) continue;
        std::array<uint8_t, 4096> data{};
        ssize_t n = read(control_out_, data.data(), data.size());
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n < 40) {
            running_ = false;
            break;
        }
        auto len = Get(data.data() + 12), tag = Get(data.data() + 32);
        if (Get(data.data()) != 0xfafafafa || Get(data.data() + 8) != 0x12 || len < 40 ||
            len > size_t(n) || Get(data.data() + 36) != len - 40) {
            running_ = false;
            break;
        }
        if (tag == 0xa002 || tag == 0xa004 || tag == 0xa00e || tag == 0xa010) Command(tag + 1);
    }
}
bool UsbLink::Send(const EncodedFrame& f) {
    std::vector<uint8_t> data(1024 + f.size, 255);
    std::memset(data.data(), 0, 32 + 116);
    Header(data.data(), 0x10, data.size());
    Put(data.data() + 16, 1024);
    auto* i = data.data() + 32;
    Put(i, f.channel);
    Put(i + 4, f.width);
    Put(i + 8, f.height);
    Put(i + 12, f.keyframe ? 0 : 1);
    Put(i + 16, f.frame_number);
    Put(i + 20, f.size);
    Put(i + 24, 1);
    Put(i + 44, 2);
    Put(i + 48, 116);
    Put(i + 52, f.channel);
    Put(i + 56, 1);
    Put(i + 60, f.channel);
    Put(i + 68, f.frame_number >> 32);
    Put(i + 72, f.timestamp_us);
    Put(i + 76, f.timestamp_us >> 32);
    Put(i + 80, (f.keyframe ? 1 : 0) | (f.parameter_sets ? 2 : 0));
    std::memcpy(data.data() + 1024, f.data.get(), f.size);
    auto notice = Notice(0, data.size());
    std::lock_guard<std::mutex> l(write_mutex_);
    bool ok = Transfer(interrupt_, notice.data(), notice.size(), true, 200) &&
        Transfer(video_, data.data(), data.size(), true, 200);
    if (!ok) running_ = false;  // A partial transfer cannot be retried as a new frame.
    return ok;
}
bool UsbLink::Receive(EncodedFrame* f) {
    std::array<uint8_t, 1024> h{};
    if (!Transfer(video_, h.data(), h.size(), false, 200)) return false;
    const auto* i = h.data() + 32;
    uint32_t size = Get(i + 20);
    if (Get(h.data()) != 0xfafafafa || Get(h.data() + 8) != 0x10 || Get(h.data() + 16) != 1024 ||
        !size || size > limits_.max_frame_bytes || Get(h.data() + 12) != 1024 + size ||
        Get(i) > 1 || Get(i + 24) != 1 || !Get(i + 4) || !Get(i + 8) || Get(i + 4) > 16384 ||
        Get(i + 8) > 16384) {
        running_ = false;
        return false;
    }
    auto data = std::make_shared<std::vector<uint8_t>>(size);
    if (!Transfer(video_, data->data(), size, false, 200)) {
        running_ = false;
        return false;
    }
    f->data = {data, data->data()};
    f->size = size;
    f->channel = Get(i);
    f->width = Get(i + 4);
    f->height = Get(i + 8);
    f->frame_number = Get(i + 16);
    f->keyframe = Get(i + 12) == 0;
    if (Get(i + 44) >= 2) {
        f->frame_number |= uint64_t(Get(i + 68)) << 32;
        f->timestamp_us = Get(i + 72) | (uint64_t(Get(i + 76)) << 32);
        f->parameter_sets = Get(i + 80) & 2;
    }
    return true;
}
}  // namespace video_transmission::detail
