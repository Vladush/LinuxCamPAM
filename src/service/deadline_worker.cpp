#include "deadline_worker.hpp"
#include "../common/ipc_protocol.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

DeadlineWorker::DeadlineWorker(std::vector<std::string> command) : command_(std::move(command)) {}
DeadlineWorker::~DeadlineWorker() { stop(); }

bool DeadlineWorker::start() {
  if (command_.empty()) return false;
  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) return false;
  linuxcampam::FileDescriptor parent(sockets[0]), child(sockets[1]);
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
  for (auto &argument : command_) arguments.push_back(argument.data());
  arguments.push_back(nullptr);
  if (!error)
    error = posix_spawn(&pid_, arguments[0], &actions, &attributes, arguments.data(), environ);
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

std::optional<std::string> DeadlineWorker::request(const std::string &message, int timeout_ms,
                                                   int client_fd) {
  using namespace linuxcampam::protocol;
  const auto deadline = _now_ms() + timeout_ms;
  pollfd client{client_fd, POLLRDHUP, 0};
  if (client_fd >= 0 && poll(&client, 1, 0) > 0 && client.revents) return std::nullopt;
  if (timeout_ms <= 0 || (pid_ < 0 && !start())) return std::nullopt;
  auto remaining = [&] { return static_cast<int>(std::max<int64_t>(0, deadline - _now_ms())); };
  std::string response;
  if (send_message(channel_.get(), message, remaining())) {
    while (remaining() > 0) {
      pollfd descriptors[] = {{channel_.get(), POLLIN, 0}, {client_fd, POLLRDHUP, 0}};
      const int ready = poll(descriptors, 2, remaining());
      if (ready < 0 && errno == EINTR) continue;
      if (ready <= 0 || descriptors[1].revents) break;
      if (descriptors[0].revents) {
        if (recv_message(channel_.get(), response, remaining()) && _now_ms() < deadline)
          return response;
        break;
      }
    }
  }
  stop();
  return std::nullopt;
}
