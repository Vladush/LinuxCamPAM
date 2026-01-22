#pragma once

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <syslog.h>

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

class Logger {
public:
  static void setLevel(LogLevel level) {
    std::scoped_lock lock(mutex_);
    current_level_ = level;
  }

  static LogLevel getLevel() {
    // atomic read would be better, but mutex is fine for now
    std::scoped_lock lock(mutex_);
    return current_level_;
  }

  static void setLogFile(const std::string &path) {
    std::scoped_lock lock(mutex_);
    if (log_file_.is_open()) {
      log_file_.close();
    }
    log_file_.open(path, std::ios::app);
    if (!log_file_.is_open()) {
      std::cerr << "[Logger] Failed to open log file: " << path << std::endl;
    }
  }

  static void enableSyslog(const std::string &ident) {
    std::scoped_lock lock(mutex_);
    syslog_enabled_ = true;
    openlog(ident.c_str(), LOG_PID | LOG_NDELAY, LOG_DAEMON);
  }

  static void log(LogLevel level, const std::string &msg) {
    std::scoped_lock lock(mutex_);
    if (level < current_level_) {
      return;
    }

    // Output to syslog if enabled
    if (syslog_enabled_) {
      int priority = LOG_INFO;
      switch (level) {
      case LogLevel::DEBUG:
        priority = LOG_DEBUG;
        break;
      case LogLevel::INFO:
        priority = LOG_INFO;
        break;
      case LogLevel::WARN:
        priority = LOG_WARNING;
        break;
      case LogLevel::ERROR:
        priority = LOG_ERR;
        break;
      }
      syslog(priority, "%s", msg.c_str());
    }

    // Get timestamp
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");

    std::string levelStr;
    switch (level) {
    case LogLevel::DEBUG:
      levelStr = "[DEBUG]";
      break;
    case LogLevel::INFO:
      levelStr = "[INFO ]";
      break;
    case LogLevel::WARN:
      levelStr = "[WARN ]";
      break;
    case LogLevel::ERROR:
      levelStr = "[ERROR]";
      break;
    }

    std::string fullMsg = ss.str() + " " + levelStr + " " + msg;

    // Output to stdout/stderr
    if (level >= LogLevel::ERROR) {
      std::cerr << fullMsg << std::endl;
    } else {
      std::cout << fullMsg << std::endl;
    }

    // Output to file if open
    if (log_file_.is_open()) {
      log_file_ << fullMsg << std::endl;
    }
  }

private:
  static inline std::mutex mutex_;
  static inline LogLevel current_level_ = LogLevel::INFO;
  static inline std::ofstream log_file_;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static inline bool syslog_enabled_ = false;
};

// Helper functions to replace macros
// These check the level first to avoid unnecessary logging overhead
template <typename T>
inline void log_debug(const T &msg) {
  if (Logger::getLevel() <= LogLevel::DEBUG) {
    Logger::log(LogLevel::DEBUG, msg);
  }
}

template <typename T>
inline void log_info(const T &msg) {
  if (Logger::getLevel() <= LogLevel::INFO) {
    Logger::log(LogLevel::INFO, msg);
  }
}

template <typename T>
inline void log_warn(const T &msg) {
  if (Logger::getLevel() <= LogLevel::WARN) {
    Logger::log(LogLevel::WARN, msg);
  }
}

template <typename T>
inline void log_error(const T &msg) {
  if (Logger::getLevel() <= LogLevel::ERROR) {
    Logger::log(LogLevel::ERROR, msg);
  }
}

// Macros removed to avoid conflict with syslog.h
// Use log_debug(), log_info(), log_warn(), log_error() directly.
