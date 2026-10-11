#include "deadline_worker.hpp"
#include "../common/ipc_protocol.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

DeadlineWorker::DeadlineWorker(std::vector<std::string> command) : command_(std::move(command)) {}
DeadlineWorker::~DeadlineWorker() { stop(); }

bool DeadlineWorker::start() {
  if (command_.empty()) return false;
  std::array<int, 2> sockets{};
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) != 0) return false;
  linuxcampam::FileDescriptor parent(sockets.at(0)), child(sockets.at(1));
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) return false;
  posix_spawnattr_t attributes;
  if (posix_spawnattr_init(&attributes) != 0) {
    posix_spawn_file_actions_destroy(&actions);
    return false;
  }
  int error = posix_spawn_file_actions_adddup2(&actions, child.get(), CHANNEL_FD);
  if (!error && parent.get() != CHANNEL_FD)
    error = posix_spawn_file_actions_addclose(&actions, parent.get());
  if (!error && child.get() != CHANNEL_FD)
    error = posix_spawn_file_actions_addclose(&actions, child.get());
  if (!error) error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  if (!error) error = posix_spawnattr_setpgroup(&attributes, 0);
  std::vector<char *> arguments;
  arguments.reserve(command_.size() + 1);
  for (size_t i = 0; i < command_.size(); ++i) {
    arguments.push_back(command_.at(i).data());
  }
  arguments.push_back(nullptr);
  if (!error)
    error = posix_spawn(&pid_, arguments.at(0), &actions, &attributes, arguments.data(), environ);
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  if (error) { pid_ = -1; return false; }
  channel_ = std::move(parent);
  return true;
}

void DeadlineWorker::stop() {
  channel_ = linuxcampam::FileDescriptor{};
  if (pid_ > 0) {
    // The child cannot outlive its supervisor or retain the camera after timeout.
    (void)kill(-pid_, SIGKILL);
    while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
    pid_ = -1;
  }
}

std::optional<std::string> DeadlineWorker::request(const std::string &message,
                                                   std::chrono::milliseconds timeout,
                                                   int client_fd) {
  using namespace linuxcampam::protocol;
  const auto timeout_ms = static_cast<int64_t>(timeout.count());
  const auto deadline = _now_ms() + timeout_ms;
  pollfd client{client_fd, POLLRDHUP, 0};
  if (client_fd >= 0 && poll(&client, 1, 0) > 0 && client.revents) return std::nullopt;
  if (timeout_ms <= 0 || (pid_ < 0 && !start())) return std::nullopt;
  auto remaining = [&] { return static_cast<int>(std::max<int64_t>(0, deadline - _now_ms())); };
  if (send_message(channel_.get(), message, remaining())) {
    while (remaining() > 0) {
      std::array<pollfd, 2> descriptors{{{channel_.get(), POLLIN, 0}, {client_fd, POLLRDHUP, 0}}};
      const int ready = poll(descriptors.data(), descriptors.size(), remaining());
      if (ready < 0 && errno == EINTR) continue;
      if (ready <= 0 || descriptors.at(1).revents) break;
      if (descriptors.at(0).revents) {
        std::string response;
        if (recv_message(channel_.get(), response, remaining()) && _now_ms() < deadline)
          return response;
        break;
      }
    }
  }
  stop();
  return std::nullopt;
}
