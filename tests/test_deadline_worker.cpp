#include "service/deadline_worker.hpp"
#include "constants.hpp"
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <signal.h>
#include <thread>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "json.hpp"

#ifndef DEADLINE_WORKER_FIXTURE
#error "DEADLINE_WORKER_FIXTURE must be defined"
#endif

namespace {
constexpr int kDefaultTimeoutMs = 1000;
constexpr int kShortTimeoutMs = 100;
constexpr int kStallTimeoutMs = 50;
constexpr int kChildTimeoutMs = 200;
constexpr int kMaxTimeoutMs = 5000;
constexpr auto kPollInterval = std::chrono::milliseconds(5);
constexpr auto kDisconnectDelay = std::chrono::milliseconds(30);
constexpr auto kMaxWaitDuration = std::chrono::milliseconds(500);
constexpr auto kModelLoadThreshold = std::chrono::milliseconds(700);
} // namespace

TEST(DeadlineWorkerTest, ReusesWorkerAndReapsItOnDestruction) {
  pid_t pid = -1;
  {
    DeadlineWorker worker({DEADLINE_WORKER_FIXTURE});
    auto first = worker.request("pid", kDefaultTimeoutMs);
    ASSERT_TRUE(first.has_value());
    if (!first.has_value()) return;
    pid = std::stoi(first.value());
    EXPECT_EQ(worker.request("pid", kDefaultTimeoutMs), first);
  }
  EXPECT_EQ(kill(pid, 0), -1);
  EXPECT_EQ(errno, ESRCH);
}

TEST(DeadlineWorkerTest, TimeoutTerminatesBlockingWorkAndNextRequestRecovers) {
  DeadlineWorker worker({DEADLINE_WORKER_FIXTURE});
  auto first = worker.request("pid", kDefaultTimeoutMs);
  ASSERT_TRUE(first.has_value());
  if (!first.has_value()) return;
  auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(worker.request("stall", kStallTimeoutMs));
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, kMaxWaitDuration);
  EXPECT_EQ(kill(std::stoi(first.value()), 0), -1);
  auto second = worker.request("pid", kDefaultTimeoutMs);
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(first, second);
}

TEST(DeadlineWorkerTest, CrashAndMissingExecutableFailClosed) {
  DeadlineWorker worker({DEADLINE_WORKER_FIXTURE});
  EXPECT_FALSE(worker.request("exit", kDefaultTimeoutMs));
  EXPECT_TRUE(worker.request("pid", kDefaultTimeoutMs));
  DeadlineWorker missing({"/nonexistent/linuxcampam-worker"});
  EXPECT_FALSE(missing.request("pid", kShortTimeoutMs));
}

TEST(DeadlineWorkerTest, TimeoutAlsoTerminatesDescendants) {
  int previous = 0;
  ASSERT_EQ(prctl(PR_GET_CHILD_SUBREAPER, &previous), 0);
  ASSERT_EQ(prctl(PR_SET_CHILD_SUBREAPER, 1), 0);
  struct RestoreSubreaper {
    int value;
    RestoreSubreaper(const RestoreSubreaper &) = delete;
    RestoreSubreaper &operator=(const RestoreSubreaper &) = delete;
    RestoreSubreaper(RestoreSubreaper &&) = delete;
    RestoreSubreaper &operator=(RestoreSubreaper &&) = delete;
    ~RestoreSubreaper() { (void)prctl(PR_SET_CHILD_SUBREAPER, value); }
  } restore{previous};
  const auto path = std::filesystem::temp_directory_path() /
                    ("linuxcampam-descendant-" + std::to_string(getpid()));
  DeadlineWorker worker({DEADLINE_WORKER_FIXTURE});
  EXPECT_FALSE(worker.request("child " + path.string(), kChildTimeoutMs));
  pid_t child = -1;
  { std::ifstream file(path); file >> child; }
  std::filesystem::remove(path);
  ASSERT_GT(child, 0);
  int status = 0;
  pid_t reaped = 0;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (std::chrono::steady_clock::now() < until) {
    reaped = waitpid(child, &status, WNOHANG);
    if (reaped != 0) break;
    std::this_thread::sleep_for(kPollInterval);
  }
  EXPECT_EQ(reaped, child);
  if (reaped == 0) { (void)kill(child, SIGKILL); (void)waitpid(child, &status, 0); }
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGKILL);
}

TEST(DeadlineWorkerTest, ActualVerificationWorkerBoundsModelLoadingAndCapture) {
#ifdef LINUXCAMPAM_TEST_MODELS_DIR
  const std::filesystem::path test_models_dir(LINUXCAMPAM_TEST_MODELS_DIR);
  if (!std::filesystem::exists(test_models_dir / linuxcampam::DETECTOR_MODEL_FILE) &&
      !std::filesystem::exists(std::filesystem::path(linuxcampam::MODELS_DIR) / linuxcampam::DETECTOR_MODEL_FILE)) {
    GTEST_SKIP() << "AI models not found, skipping deadline worker test";
  }
#endif
  std::string pattern = "/tmp/linuxcampam-worker-test-XXXXXX";
  char *directory = mkdtemp(pattern.data());
  ASSERT_NE(directory, nullptr);
  const std::filesystem::path root(directory);
  struct Cleanup {
    std::filesystem::path path;
    Cleanup(const Cleanup &) = delete;
    Cleanup &operator=(const Cleanup &) = delete;
    Cleanup(Cleanup &&) = delete;
    Cleanup &operator=(Cleanup &&) = delete;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{root};
  const auto config = root / "config.ini";
  { std::ofstream file(config);
    file << "[Paths]\nusers_dir=" << root.string()
#ifdef LINUXCAMPAM_TEST_MODELS_DIR
         << "\nmodels_dir=" << LINUXCAMPAM_TEST_MODELS_DIR
#endif
         << "\n[Cameras]\nnames=invalid\n[Camera.invalid]\npath=/dev/video9999\ntype=rgb"
         << "\n[Hardware]\nprovider_priority=CPU\n[Auth]\ntimeout_ms=100\n";
  }
  { std::ofstream file(root / "enrolled.json"); file << "{}"; }
  DeadlineWorker worker({LINUXCAMPAMD_EXECUTABLE, "--verification-worker", config.string(), std::to_string(getpid())});
  const auto start = std::chrono::steady_clock::now();
  nlohmann::json request = {{"user", "enrolled"}, {"diagnostic", true}, {"log_level", 3}};
  EXPECT_FALSE(worker.request(request.dump(), kShortTimeoutMs));
  EXPECT_LT(std::chrono::steady_clock::now() - start, kModelLoadThreshold);
}

TEST(DeadlineWorkerTest, ClientDisconnectCancelsVerificationBeforeDeadline) {
  std::array<int, 2> sockets{};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()), 0);
  linuxcampam::FileDescriptor client(sockets.at(0)), peer(sockets.at(1));
  DeadlineWorker worker({DEADLINE_WORKER_FIXTURE});
  auto pid = worker.request("pid", kDefaultTimeoutMs);
  ASSERT_TRUE(pid.has_value());
  if (!pid.has_value()) return;
  std::thread disconnect([&] {
    std::this_thread::sleep_for(kDisconnectDelay);
    (void)shutdown(peer.get(), SHUT_RDWR);
  });
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(worker.request("stall", std::chrono::milliseconds(kMaxTimeoutMs), client.get()));
  disconnect.join();
  EXPECT_LT(std::chrono::steady_clock::now() - start, kMaxWaitDuration);
  EXPECT_EQ(kill(std::stoi(pid.value()), 0), -1);
}
