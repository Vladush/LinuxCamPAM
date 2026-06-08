#pragma once

#include "SensorParser.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <hidapi.h>
#include "HardwareManager.hpp"
#include "ScopedWorker.hpp"
#include "SensorFactory.hpp"

class PresenceTripwire {
public:
    using PresenceCallback = std::function<void(bool present, int distance)>;

    explicit PresenceTripwire(const ISensorFactory& factory);
    ~PresenceTripwire() noexcept;

    // Prevent copy/move semantics for daemon thread safety
    PresenceTripwire(const PresenceTripwire&) = delete;
    PresenceTripwire& operator=(const PresenceTripwire&) = delete;
    PresenceTripwire(PresenceTripwire&&) = delete;
    PresenceTripwire& operator=(PresenceTripwire&&) = delete;

    [[nodiscard]] bool start(const std::string& hidraw_node, const HardwareId& hardware_id, const PresenceCallback& cb) noexcept;
    void stop() noexcept;

private:
    void poll_loop() noexcept;

    const ISensorFactory& factory_;
    PresenceCallback callback_;
    std::unique_ptr<SensorParser> parser_;
    std::atomic<bool> running_{false};
    ScopedWorker worker_;
    std::unique_ptr<hid_device, decltype(&hid_close)> handle_{nullptr, hid_close};
};
