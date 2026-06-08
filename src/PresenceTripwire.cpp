#include "PresenceTripwire.hpp"
#include "SensorFactory.hpp"
#include <array>

#include <hidapi.h>
#include "service/logger.hpp"

PresenceTripwire::PresenceTripwire(const ISensorFactory& factory) : factory_(factory) {}

PresenceTripwire::~PresenceTripwire() noexcept {
    stop();
}

bool PresenceTripwire::start(const std::string& hidraw_node, const HardwareId& hardware_id, const PresenceCallback& cb) noexcept {
    if (running_) return false;

    try {
        parser_ = factory_.create(hardware_id.str());
    } catch (...) {
        return false;
    }
    if (!parser_) return false;

    callback_ = cb;

    if (hid_init() != 0) return false;

    handle_.reset(hid_open_path(hidraw_node.c_str()));
    if (!handle_) {
        hid_exit();
        return false;
    }

    running_ = true;
    try {
        worker_ = ScopedWorker(&PresenceTripwire::poll_loop, this);
    } catch (...) {
        running_ = false;
        handle_.reset();
        hid_exit();
        return false;
    }
    return true;
}

void PresenceTripwire::stop() noexcept {
    if (running_) {
        running_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }
        handle_.reset();
        hid_exit();
    }
}

void PresenceTripwire::poll_loop() noexcept {
    constexpr size_t READ_BUFFER_SIZE = 64; // Ample buffer size for reading the incoming I2C HID payload
    constexpr int POLL_TIMEOUT_MS = 500; // Timeout to ensure the running flag is frequently checked during polling

    std::array<uint8_t, READ_BUFFER_SIZE> buffer{};

    // Note: Thread isolation is maintained here intentionally. While the main application 
    // uses an event loop (select) for UNIX sockets, dedicating a separate thread for the 
    // HID sensor prevents a malfunctioning or blocking hardware device from locking up 
    // the critical authentication pathways in the main event loop.
    while (running_) {
        // hid_read_timeout handles the polling for us. It will return 0 on timeout.
        int bytes = hid_read_timeout(handle_.get(), buffer.data(), buffer.size(), POLL_TIMEOUT_MS);
        
        if (bytes > 0) {
            // log_debug("HID read " + std::to_string(bytes) + " bytes");
            if (auto state = parser_->parse_payload(buffer.data(), static_cast<size_t>(bytes))) {
                if (callback_) {
                    callback_(state->human_present, state->confidence_cm);
                }
            } else {
                log_debug("HID payload failed to parse.");
            }
        } else if (bytes < 0) {
            log_error("HID read failed, terminating polling thread.");
            running_ = false; // Cleanly abort the polling thread
            break;
        }
    }
}
