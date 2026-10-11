#include "service/deadline_worker.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <thread>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "json.hpp"

#ifndef DEADLINE_WORKER_FIXTURE
#error "DEADLINE_WORKER_FIXTURE must be defined"
#endif

TEST(DeadlineWorkerTest, ReusesWorkerAndReapsItOnDestruction) {
  pid_t pid = -1;
  {
    DeadlineWorker worker({DEADLINE_WORKER_FIXTURE});
    auto first = worker.request("pid", 1000);
    ASSERT_TRUE(first);
    pid = std::stoi(*first);
    EXPECT_EQ(worker.request("pid", 1000), first);
  }
  EXPECT_EQ(kill(pid, 0), -1);
  EXPECT_EQ(errno, ESRCH);
}

TEST(DeadlineWorkerTest, TimeoutTerminatesBlockingWorkAndNextRequestRecovers) {
  DeadlineWorker worker({DEADLINE_WORKER_FIXTURE});
  auto first = worker.request("pid", 1000);
  ASSERT_TRUE(first);
  auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(worker.request("stall", 50));
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, std::chrono::milliseconds(500));
  EXPECT_EQ(kill(std::stoi(*first), 0), -1);
  auto second = worker.request("pid", 1000);
  ASSERT_TRUE(second);
  EXPECT_NE(first, second);
}

TEST(DeadlineWorkerTest, CrashAndMissingExecutableFailClosed) {
  DeadlineWorker worker({DEADLINE_WORKER_FIXTURE});
  EXPECT_FALSE(worker.request("exit", 1000));
  EXPECT_TRUE(worker.request("pid", 1000));
  DeadlineWorker missing({"/nonexistent/linuxcampam-worker"});
  EXPECT_FALSE(missing.request("pid", 100));
}

TEST(DeadlineWorkerTest, TimeoutAlsoTerminatesDescendants) {
  int previous = 0;
  ASSERT_EQ(prctl(PR_GET_CHILD_SUBREAPER, &previous), 0);
  ASSERT_EQ(prctl(PR_SET_CHILD_SUBREAPER, 1), 0);
  struct RestoreSubreaper {
    int value;
    ~RestoreSubreaper() { (void)prctl(PR_SET_CHILD_SUBREAPER, value); }
  } restore{previous};
  const auto path = std::filesystem::temp_directory_path() /
                    ("linuxcampam-descendant-" + std::to_string(getpid()));
  DeadlineWorker worker({DEADLINE_WORKER_FIXTURE});
  EXPECT_FALSE(worker.request("child " + path.string(), 200));
  pid_t child = -1;
  { std::ifstream file(path); file >> child; }
  std::filesystem::remove(path);
  ASSERT_GT(child, 0);
  int status = 0;
  pid_t reaped = 0;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  do {
    reaped = waitpid(child, &status, WNOHANG);
    if (reaped != 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < until);
  EXPECT_EQ(reaped, child);
  if (reaped == 0) { (void)kill(child, SIGKILL); (void)waitpid(child, &status, 0); }
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGKILL);
}

TEST(DeadlineWorkerTest, ActualVerificationWorkerBoundsModelLoadingAndCapture) {
  char pattern[] = "/tmp/linuxcampam-worker-test-XXXXXX";
  const char *directory = mkdtemp(pattern);
  ASSERT_NE(directory, nullptr);
  const std::filesystem::path root(directory);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{root};
  const auto config = root / "config.ini";
  { std::ofstream file(config);
    file << "[Paths]\nusers_dir=" << root.string()
         << "\n[Cameras]\nnames=invalid\n[Camera.invalid]\npath=/dev/video9999\ntype=rgb"
         << "\n[Hardware]\nprovider_priority=CPU\n[Auth]\ntimeout_ms=100\n";
  }
  { std::ofstream file(root / "enrolled.json"); file << "{}"; }
  DeadlineWorker worker({LINUXCAMPAMD_EXECUTABLE, "--verification-worker", config.string(), std::to_string(getpid())});
  const auto start = std::chrono::steady_clock::now();
  nlohmann::json request = {{"user", "enrolled"}, {"diagnostic", true}, {"log_level", 3}};
  EXPECT_FALSE(worker.request(request.dump(), 100));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(700));
}

TEST(DeadlineWorkerTest, ClientDisconnectCancelsVerificationBeforeDeadline) {
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  linuxcampam::FileDescriptor client(sockets[0]), peer(sockets[1]);
  DeadlineWorker worker({DEADLINE_WORKER_FIXTURE});
  auto pid = worker.request("pid", 1000);
  ASSERT_TRUE(pid);
  std::thread disconnect([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    (void)shutdown(peer.get(), SHUT_RDWR);
  });
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(worker.request("stall", 5000, client.get()));
  disconnect.join();
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(500));
  EXPECT_EQ(kill(std::stoi(*pid), 0), -1);
}
