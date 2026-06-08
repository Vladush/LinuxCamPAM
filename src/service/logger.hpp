#pragma once

#include <atomic>
#include <cstdint>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <memory>
#include <syslog.h>

enum class LogLevel : std::uint8_t { DEBUG, INFO, WARN, ERROR };

class ILogger {
public:
    virtual ~ILogger() = default;
    ILogger() = default;
    ILogger(const ILogger&) = delete;
    ILogger& operator=(const ILogger&) = delete;
    ILogger(ILogger&&) = delete;
    ILogger& operator=(ILogger&&) = delete;
    virtual void log(LogLevel level, const std::string& msg) = 0;
};

class SyslogLogger : public ILogger {
    std::string ident_;
public:
    explicit SyslogLogger(std::string ident) : ident_(std::move(ident)) {
        openlog(ident_.c_str(), LOG_PID | LOG_NDELAY, LOG_DAEMON);
    }
    ~SyslogLogger() override {
        closelog();
    }
    SyslogLogger(const SyslogLogger&) = delete;
    SyslogLogger& operator=(const SyslogLogger&) = delete;
    SyslogLogger(SyslogLogger&&) = delete;
    SyslogLogger& operator=(SyslogLogger&&) = delete;
    void log(LogLevel level, const std::string& msg) override {
        int priority = LOG_INFO;
        switch (level) {
            case LogLevel::DEBUG: priority = LOG_DEBUG; break;
            case LogLevel::INFO:  priority = LOG_INFO; break;
            case LogLevel::WARN:  priority = LOG_WARNING; break;
            case LogLevel::ERROR: priority = LOG_ERR; break;
        }
        syslog(priority, "%s", msg.c_str());
    }
};

class Logger {
public:
    static void setLevel(LogLevel level) {
        current_level_.store(level, std::memory_order_relaxed);
    }

    static LogLevel getLevel() {
        return current_level_.load(std::memory_order_relaxed);
    }

    static void setLogFile(const std::string& path) {
        std::scoped_lock lock(mutex_);
        if (log_file_.is_open()) {
            log_file_.close();
        }
        log_file_.open(path, std::ios::app);
        if (!log_file_.is_open()) {
            std::cerr << "[Logger] Failed to open log file: " << path << '\n';
        }
    }

    static void enableSyslog(const std::string& ident) {
        std::scoped_lock lock(mutex_);
        syslog_logger_ = std::make_unique<SyslogLogger>(ident);
    }

    static void log(LogLevel level, const std::string& msg) {
        std::scoped_lock lock(mutex_);
        if (level < current_level_.load(std::memory_order_relaxed)) {
            return;
        }

        if (syslog_logger_) {
            syslog_logger_->log(level, msg);
        }

        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm timeinfo{};
        localtime_r(&in_time_t, &timeinfo);
        std::stringstream ss;
        ss << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S");

        std::string levelStr;
        switch (level) {
            case LogLevel::DEBUG: levelStr = "[DEBUG]"; break;
            case LogLevel::INFO:  levelStr = "[INFO ]"; break;
            case LogLevel::WARN:  levelStr = "[WARN ]"; break;
            case LogLevel::ERROR: levelStr = "[ERROR]"; break;
        }

        std::string fullMsg = ss.str() + " " + levelStr + " " + msg;

        if (level >= LogLevel::ERROR) {
            std::cerr << fullMsg << '\n';
        } else {
            std::cout << fullMsg << '\n';
        }

        if (log_file_.is_open()) {
            log_file_ << fullMsg << '\n';
        }
    }

private:
    static inline std::mutex mutex_;
    static inline std::atomic<LogLevel> current_level_{LogLevel::INFO};
    static inline std::ofstream log_file_;
    static inline std::unique_ptr<ILogger> syslog_logger_;
};

template <typename T>
constexpr void log_debug(const T& msg) {
    if (Logger::getLevel() <= LogLevel::DEBUG) {
        Logger::log(LogLevel::DEBUG, msg);
    }
}

template <typename T>
constexpr void log_info(const T& msg) {
    if (Logger::getLevel() <= LogLevel::INFO) {
        Logger::log(LogLevel::INFO, msg);
    }
}

template <typename T>
constexpr void log_warn(const T& msg) {
    if (Logger::getLevel() <= LogLevel::WARN) {
        Logger::log(LogLevel::WARN, msg);
    }
}

template <typename T>
constexpr void log_error(const T& msg) {
    if (Logger::getLevel() <= LogLevel::ERROR) {
        Logger::log(LogLevel::ERROR, msg);
    }
}
