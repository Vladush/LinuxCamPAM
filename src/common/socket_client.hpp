#pragma once

#include "resources.hpp"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>

namespace linuxcampam {
inline FileDescriptor connectUnixSocket(std::string_view path, int timeout_ms) {
  sockaddr_un address{};
  if (path.size() >= sizeof(address.sun_path)) { errno = ENAMETOOLONG; return {}; }
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.data(), path.size());
  FileDescriptor socket_fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (!socket_fd.isValid()) return {};
  if (connect(socket_fd.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
    return socket_fd;
  // AF_UNIX EAGAIN means a full backlog, not an asynchronous connection.
  if (errno != EINPROGRESS) return {};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) { errno = ETIMEDOUT; return {}; }
    pollfd pfd{socket_fd.get(), POLLOUT, 0};
    int result = poll(&pfd, 1, static_cast<int>(remaining));
    if (result < 0 && errno == EINTR) continue;
    if (result <= 0) { if (result == 0) errno = ETIMEDOUT; return {}; }
    int error = 0;
    socklen_t size = sizeof(error);
    if (getsockopt(socket_fd.get(), SOL_SOCKET, SO_ERROR, &error, &size) != 0) return {};
    if (error) { errno = error; return {}; }
    return socket_fd;
  }
}
} // namespace linuxcampam
