#pragma once

#include "config.hpp"
#include "logger.hpp"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

// The lock deadline is independent of the socket/camera thread. Callbacks run
// without the state mutex so a lock command cannot hold up sensor readings.
class PresenceActions {
public:
  using Action = std::function<void()>;
  PresenceActions(const Configuration &config, Action wake, Action lock)
      : config_(config), wake_(std::move(wake)), lock_(std::move(lock)),
        worker_([this] { run(); }) {}
  ~PresenceActions() {
    { std::lock_guard<std::mutex> guard(mutex_); stopping_ = true; }
    changed_.notify_all();
    worker_.join();
  }
  PresenceActions(const PresenceActions &) = delete;
  PresenceActions &operator=(const PresenceActions &) = delete;

  void observe(bool present, int confidence) {
    bool wake = false;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      wake = present && config_.wake_enabled && confidence >= config_.wake_confidence_threshold &&
             (away_ || (config_.always_wake_on_presence_detected && !last_present_));
      last_present_ = present;
      if (present && confidence >= config_.lock_confidence_threshold) {
        // Tracking presence must work even when wake_enabled is false.
        away_ = false;
        deadline_.reset();
      } else if (config_.lock_enabled) {
        if (!away_ && !deadline_)
          deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(config_.lock_timeout_seconds);
      } else {
        away_ = true;
      }
    }
    changed_.notify_all();
    if (wake) wake_();
  }

private:
  void run() {
    std::unique_lock<std::mutex> guard(mutex_);
    while (!stopping_) {
      if (!deadline_) {
        changed_.wait(guard, [this] { return stopping_ || deadline_.has_value(); });
        continue;
      }
      const auto deadline = *deadline_;
      if (changed_.wait_until(guard, deadline, [this, deadline] {
            return stopping_ || !deadline_ || *deadline_ != deadline;
          })) continue;
      away_ = true;
      deadline_.reset();
      guard.unlock();
      try { lock_(); } catch (const std::exception &error) { log_error(error.what()); }
      guard.lock();
    }
  }
  const Configuration config_;
  Action wake_;
  Action lock_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool stopping_ = false;
  bool away_ = true;
  bool last_present_ = false;
  std::optional<std::chrono::steady_clock::time_point> deadline_;
  std::thread worker_;
};
