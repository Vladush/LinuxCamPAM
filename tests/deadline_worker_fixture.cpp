#include "common/ipc_protocol.hpp"
#include "service/deadline_worker.hpp"
#include <fstream>
#include <fcntl.h>
#include <sys/wait.h>
#include <csignal>

int main() {
  if (fcntl(DeadlineWorker::CHANNEL_FD, F_SETFD, FD_CLOEXEC) < 0) return 1;
  for (;;) {
    std::string request;
    if (!linuxcampam::protocol::recv_message(DeadlineWorker::CHANNEL_FD, request, 5000)) return 0;
    if (request == "exit") return 1;
    if (request == "stall") { for (;;) pause(); }
    if (request.rfind("child ", 0) == 0) {
      // Make descendant termination observable without leaving orphan zombies.
      const pid_t child = fork();
      if (child < 0) return 1;
      if (child == 0) {
        std::ofstream file(request.substr(6));
        file << getpid();
        file.close();
        for (;;) pause();
      }
      for (;;) pause();
    }
    if (!linuxcampam::protocol::send_message(DeadlineWorker::CHANNEL_FD, std::to_string(getpid()), 2000)) return 0;
  }
}
