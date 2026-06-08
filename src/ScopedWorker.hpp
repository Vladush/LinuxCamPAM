#pragma once

#include <thread>
#include <utility>

class ScopedWorker {
    std::thread thread_;

public:
    ScopedWorker() noexcept = default;

    template<typename Function, typename... Args>
    explicit ScopedWorker(Function&& f, Args&&... args)
        : thread_(std::forward<Function>(f), std::forward<Args>(args)...) {}

    ~ScopedWorker() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    ScopedWorker(const ScopedWorker&) = delete;
    ScopedWorker& operator=(const ScopedWorker&) = delete;

    ScopedWorker(ScopedWorker&& other) noexcept : thread_(std::move(other.thread_)) {}

    ScopedWorker& operator=(ScopedWorker&& other) noexcept {
        if (this != &other) {
            if (thread_.joinable()) {
                thread_.join();
            }
            thread_ = std::move(other.thread_);
        }
        return *this;
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool joinable() const noexcept {
        return thread_.joinable();
    }
    
    std::thread::id get_id() const noexcept {
        return thread_.get_id();
    }
};
