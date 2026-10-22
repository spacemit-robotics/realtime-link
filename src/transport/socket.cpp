#include "socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <thread>
namespace video_transmission::detail {
int OpenSocket(const SessionConfig& c, int type, bool listener, uint16_t port) {
    int result = -1;
    // Never change the caller's namespace (it may host ROS executors or MPP).
    std::thread creator([&] {
        if (!c.netns.empty()) {
            int ns = open(c.netns.c_str(), O_RDONLY | O_CLOEXEC);
            if (ns < 0) return;
            int rc = setns(ns, CLONE_NEWNET);
            close(ns);
            if (rc < 0) return;
        }
        int fd = socket(AF_INET, type | SOCK_CLOEXEC, 0);
        if (fd < 0) return;
        auto fail = [&] { close(fd); };
        if (!c.interface.empty() &&
            setsockopt(
                fd, SOL_SOCKET, SO_BINDTODEVICE, c.interface.c_str(), c.interface.size() + 1) < 0) {
            fail();
            return;
        }
        if (listener && type == SOCK_STREAM) {
            int yes = 1;
            if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
                fail();
                return;
            }
        }
        const auto& ip = listener ? c.address : c.local_address;
        if (listener || !ip.empty()) {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(port ? port : (listener ? c.port : 0));
            if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1 ||
                bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
                fail();
                return;
            }
        }
        timeval timeout{0, 100000};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        result = fd;
    });
    creator.join();
    return result;
}
}  // namespace video_transmission::detail
