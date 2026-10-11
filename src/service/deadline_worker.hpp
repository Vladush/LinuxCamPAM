#pragma once

#include "../common/resources.hpp"
#include <optional>
#include <string>
#include <vector>
#include <sys/types.h>

// Serial requests to a persistent exec'd worker. A timeout kills its entire
// process group (including IR helper children) before another request can run.
class DeadlineWorker {
public:
  static constexpr int CHANNEL_FD = 3;
  explicit DeadlineWorker(std::vector<std::string> command);
  ~DeadlineWorker();
  DeadlineWorker(const DeadlineWorker &) = delete;
  DeadlineWorker &operator=(const DeadlineWorker &) = delete;
  [[nodiscard]] std::optional<std::string> request(const std::string &message, int timeout_ms,
                                                  int client_fd = -1);
private:
  bool start();
  void stop();
  std::vector<std::string> command_;
  linuxcampam::FileDescriptor channel_;
  pid_t pid_ = -1;
};
