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
    std::lock_guard<std::mutex> lock(mutex_);
    current_level_ = level;
  }

  static void setLogFile(const std::string &path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_file_.is_open()) {
      log_file_.close();
    }
    log_file_.open(path, std::ios::app);
    if (!log_file_.is_open()) {
      std::cerr << "[Logger] Failed to open log file: " << path << std::endl;
    }
  }

  static void log(LogLevel level, const std::string &msg) {
    std::lock_guard<std::mutex> lock(mutex_);
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
