#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <video_transmission/control.hpp>

#include "transport/socket.hpp"
namespace video_transmission {
namespace {
using Clock = std::chrono::steady_clock;
void Put(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = v >> (8 * i);
}
uint32_t Get(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
bool IO(int fd, void* ptr, size_t size, bool write, Clock::time_point deadline,
    const std::atomic<bool>& run) {
    auto* p = static_cast<uint8_t*>(ptr);
    while (size && run && Clock::now() < deadline) {
        pollfd f{fd, short(write ? POLLOUT : POLLIN), 0};
        int n = poll(&f, 1, 20);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (f.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
        if (!n) continue;
        ssize_t done = write ? send(fd, p, size, MSG_NOSIGNAL | MSG_DONTWAIT)
                            : recv(fd, p, size, MSG_DONTWAIT);
        if (done < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (done <= 0) return false;
        p += done;
        size -= done;
    }
    return size == 0;
}
int Connect(const SessionConfig& c, const std::string& ip, uint16_t port,
    Clock::time_point deadline, const std::atomic<bool>& run) {
    int fd = detail::OpenSocket(c, SOCK_STREAM, false);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    int rc = connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    if (rc == 0) return fd;
    if (errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    while (run && Clock::now() < deadline) {
        pollfd p{fd, POLLOUT, 0};
        if (poll(&p, 1, 20) > 0) {
            int error = 0;
            socklen_t n = sizeof(error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &n) == 0 && !error) return fd;
            break;
        }
    }
    close(fd);
    return -1;
}
bool Send(int fd, const ControlMessage& m, Clock::time_point end, const std::atomic<bool>& run) {
    std::vector<uint8_t> b(40 + m.payload.size());
    Put(b.data(), 0xfafafafa);
    Put(b.data() + 4, 0x01000000);
    Put(b.data() + 8, 0x12);
    Put(b.data() + 12, b.size());
    Put(b.data() + 32, m.tag);
    Put(b.data() + 36, m.payload.size());
    std::copy(m.payload.begin(), m.payload.end(), b.begin() + 40);
    return IO(fd, b.data(), b.size(), true, end, run);
}
bool Receive(int fd, ControlMessage* m, Clock::time_point end, const std::atomic<bool>& run) {
    std::array<uint8_t, 40> b{};
    if (!IO(fd, b.data(), b.size(), false, end, run)) return false;
    auto size = Get(b.data() + 36);
    if (Get(b.data()) != 0xfafafafa || Get(b.data() + 8) != 0x12 || size > 4096 ||
        Get(b.data() + 12) != 40 + size)
        return false;
    m->tag = Get(b.data() + 32);
    m->payload.resize(size);
    return IO(fd, m->payload.data(), size, false, end, run);
}
std::future<ControlReply> Ready(ControlStatus s) {
    std::promise<ControlReply> p;
    p.set_value({s, {}});
    return p.get_future();
}
}  // namespace
struct ControlSession::Impl {
    struct Request {
        ControlMessage message;
        Clock::time_point deadline;
        std::promise<ControlReply> reply;
    };
    FpvConfig config;
    SessionConfig link;
    std::atomic<bool> run{false};
    int listener = -1;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Request> queue;
    std::function<ControlMessage(const ControlMessage&)> handler;
    void Client() {
        int fd = -1;
        while (run) {
            Request r;
            {
                std::unique_lock l(mutex);
                cv.wait_for(
                    l, std::chrono::milliseconds(50), [&] { return !run || !queue.empty(); });
                if (!run) break;
                if (queue.empty()) continue;
                r = std::move(queue.front());
                queue.pop_front();
            }
            if (fd < 0)
                fd = Connect(
                    link, config.connections.sky.device_ip, config.control_port, r.deadline, run);
            ControlReply out;
            if (fd >= 0 && Clock::now() < r.deadline) {
                out.status = ControlStatus::Unknown;  // A partial send already makes a
                                                      // non-idempotent result unknown.
                if (Send(fd, r.message, r.deadline, run) &&
                    Receive(fd, &out.message, r.deadline, run) &&
                    out.message.tag == r.message.tag + 1) {
                    out.status = ControlStatus::Applied;
                    if (out.message.payload.size() >= 8 &&
                        Get(out.message.payload.data() + 4) != 0 && r.message.tag != 0xa006)
                        out.status = ControlStatus::Rejected;
                } else {
                    close(fd);
                    fd = -1;
                }
            }
            if (!run && out.status == ControlStatus::NotSent) out.status = ControlStatus::Cancelled;
            r.reply.set_value(std::move(out));
        }
        if (fd >= 0) close(fd);
    }
    void Server() {
        int fd = -1;
        while (run) {
            if (fd < 0) {
                pollfd p{listener, POLLIN, 0};
                if (poll(&p, 1, 50) <= 0) continue;
                fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (fd < 0) continue;
            }
            pollfd p{fd, POLLIN, 0};
            int ready = poll(&p, 1, 50);
            if (ready <= 0) continue;
            ControlMessage request;
            auto end = Clock::now() + std::chrono::seconds(1);
            if (!Receive(fd, &request, end, run)) {
                close(fd);
                fd = -1;
                continue;
            }
            ControlMessage response{request.tag + 1, {}};
            try {
                if (handler)
                    response = handler(request);
                else {
                    response.payload.resize(136);
                    Put(response.payload.data() + 4, uint32_t(-1));
                }
            } catch (...) {
                response.payload.resize(136);
                Put(response.payload.data() + 4, uint32_t(-1));
            }
            if (response.payload.size() > 4096 || !Send(fd, response, end, run)) {
                close(fd);
                fd = -1;
            }
        }
        if (fd >= 0) close(fd);
    }
};
ControlSession::ControlSession() : impl_(new Impl) {}
ControlSession::~ControlSession() { Stop(); }
bool ControlSession::Start(
    const FpvConfig& c, std::function<ControlMessage(const ControlMessage&)> handler) {
    auto& s = *impl_;
    if (s.run) return false;
    s.config = c;
    s.link = ResolveLink(c);
    s.handler = std::move(handler);
    if (c.role == Role::Sky) {
        s.link.address = c.connections.sky.device_ip;
        s.listener = detail::OpenSocket(s.link, SOCK_STREAM, true, c.control_port);
        if (s.listener < 0) return false;
        if (listen(s.listener, 1)) {
            close(s.listener);
            s.listener = -1;
            return false;
        }
    }
    s.run = true;
    s.worker = std::thread([&s] {
        if (s.config.role == Role::Sky)
            s.Server();
        else
            s.Client();
    });
    return true;
}
std::future<ControlReply> ControlSession::SendControl(
    ControlMessage m, std::chrono::milliseconds timeout) {
    auto& s = *impl_;
    std::lock_guard l(s.mutex);
    if (!s.run || s.config.role != Role::Ground) return Ready(ControlStatus::NotSent);
    if (m.payload.size() > 4096 || timeout.count() <= 0) return Ready(ControlStatus::Rejected);
    if (s.queue.size() >= 16) return Ready(ControlStatus::QueueFull);
    Impl::Request r{std::move(m), Clock::now() + timeout, {}};
    auto f = r.reply.get_future();
    s.queue.push_back(std::move(r));
    s.cv.notify_all();
    return f;
}
void ControlSession::Stop() {
    auto& s = *impl_;
    s.run = false;
    s.cv.notify_all();
    if (s.worker.joinable()) s.worker.join();
    if (s.listener >= 0) close(s.listener);
    s.listener = -1;
    std::lock_guard l(s.mutex);
    for (auto& r : s.queue) r.reply.set_value({ControlStatus::Cancelled, {}});
    s.queue.clear();
}
ControlReply ExecuteModuleAt(const FpvConfig& c, const std::string& cmd, const std::string& user,
    const std::string& password, std::chrono::milliseconds timeout) {
    if (cmd.empty() || cmd.size() > 1024 || cmd.find_first_of("\r\n") != std::string::npos ||
        user.find_first_of("\r\n,") != std::string::npos ||
        password.find_first_of("\r\n,") != std::string::npos)
        return {ControlStatus::Rejected, {}};
    std::atomic<bool> run{true};
    auto end = Clock::now() + timeout;
    auto& local = c.role == Role::Sky ? c.connections.sky : c.connections.ground;
    int fd = Connect(ResolveLink(c), local.module_ip, c.module_port, end, run);
    if (fd < 0) return {};
    struct Close {
        int fd;
        ~Close() { close(fd); }
    } close_fd{fd};
    auto exchange = [&](const std::string& text, std::string& out) {
        auto line = text + "\r";
        if (!IO(fd, line.data(), line.size(), true, end, run)) return false;
        char ch;
        while (out.size() < 8192 && IO(fd, &ch, 1, false, end, run)) {
            out += ch;
            if (out.ends_with("OK\r\n") || out.ends_with("OK\r")) return true;
            if (out.find("ERROR") != std::string::npos) return false;
        }
        return false;
    };
    std::string reply;
    if (!user.empty() && !exchange("AT+AUTH=" + user + "," + password, reply))
        return {ControlStatus::Rejected, {}};
    reply.clear();
    bool ok = exchange(cmd, reply);
    ControlReply r;
    r.status = ok ? ControlStatus::Applied : ControlStatus::Unknown;
    r.message.payload.assign(reply.begin(), reply.end());
    return r;
}
}  // namespace video_transmission
