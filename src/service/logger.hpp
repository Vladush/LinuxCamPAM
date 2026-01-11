#pragma once

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>

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

  static void log(LogLevel level, const std::string &msg) {
    std::scoped_lock lock(mutex_);
    if (level < current_level_) {
      return;
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
};

// Logging Macros
// These macros ensure that we don't pay the cost of string construction
// or function calls if the log level is not met.
//
// Usage: LOG_DEBUG("Value: " + std::to_string(x));
//
#define LOG_DEBUG(msg)                                                         \
  if (Logger::getLevel() <= LogLevel::DEBUG)                                   \
  Logger::log(LogLevel::DEBUG, msg)

#define LOG_INFO(msg)                                                          \
  if (Logger::getLevel() <= LogLevel::INFO)                                    \
  Logger::log(LogLevel::INFO, msg)

#define LOG_WARN(msg)                                                          \
  if (Logger::getLevel() <= LogLevel::WARN)                                    \
  Logger::log(LogLevel::WARN, msg)

#define LOG_ERROR(msg)                                                         \
  if (Logger::getLevel() <= LogLevel::ERROR)                                   \
  Logger::log(LogLevel::ERROR, msg)
