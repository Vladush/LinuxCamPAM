#include "PresenceTripwire.hpp"
#include "SensorFactory.hpp"
#include <array>

#include <hidapi.h>
#include "service/logger.hpp"

PresenceTripwire::PresenceTripwire(const ISensorFactory& factory, HidOps hid_ops)
    : factory_(factory),
      hid_ops_(std::move(hid_ops)),
      handle_(nullptr, hid_ops_.close_fn) {}

PresenceTripwire::~PresenceTripwire() noexcept {
    stop();
}

bool PresenceTripwire::start(const std::string& hidraw_node,
                              const HardwareId& hardware_id,
                              const PresenceCallback& cb) noexcept {
    if (running_) return false;

    try {
        parser_ = factory_.create(hardware_id.str());
    } catch (...) {
        return false;
    }
    if (!parser_) return false;

    callback_ = cb;

    if (hid_ops_.init() != 0) return false;

    handle_.reset(hid_ops_.open_path(hidraw_node.c_str()));
    if (!handle_) {
        hid_ops_.exit_fn();
        return false;
    }

    running_ = true;
    try {
        worker_ = ScopedWorker(&PresenceTripwire::poll_loop, this);
    } catch (...) {
        running_ = false;
        handle_.reset();
        hid_ops_.exit_fn();
        return false;
    }
    return true;
}

void PresenceTripwire::stop() noexcept {
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
    // Handle might outlive running_ if poll_loop exits early on error.
    if (handle_) {
        handle_.reset();
        hid_ops_.exit_fn();
    }
}

void PresenceTripwire::poll_loop() noexcept {
    constexpr size_t READ_BUFFER_SIZE = 64;
    constexpr int POLL_TIMEOUT_MS = 500;

    std::array<uint8_t, READ_BUFFER_SIZE> buffer{};

    // Polling in a separate thread prevents hardware timeouts from blocking the main auth loop.
    while (running_) {
        int bytes = hid_ops_.read_timeout(
            handle_.get(), buffer.data(), buffer.size(), POLL_TIMEOUT_MS);

        if (bytes > 0) {
            if (auto state = parser_->parse_payload(buffer.data(),
                                                    static_cast<size_t>(bytes))) {
                if (callback_) {
                    callback_(state->human_present, state->confidence_cm);
                }
            } else {
                log_debug("HID payload failed to parse.");
            }
        } else if (bytes < 0) {
            log_error("HID read failed, terminating polling thread.");
            running_ = false;
            break;
        }
    }
}
