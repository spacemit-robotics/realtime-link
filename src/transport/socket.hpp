#ifndef SOCKET_HPP
#define SOCKET_HPP

#include <video_transmission/session.hpp>
namespace video_transmission::detail {
int OpenSocket(const SessionConfig&, int type, bool bind_listener, uint16_t port = 0);
}

#endif  // SOCKET_HPP
