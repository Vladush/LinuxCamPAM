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

// hidapi function hooks. Defaults forward to the real hidapi library.
// Tests inject mocks to exercise poll_loop() without real hardware.
//
// CONTRACT: every callable MUST NOT throw. They are invoked from noexcept
// contexts (poll_loop, destructor chain). std::function cannot enforce this
// at compile time, so a throwing callable will call std::terminate via the
// noexcept boundary — the same behaviour as a throwing destructor. The
// default lambdas wrap C functions and satisfy this contract inherently.
struct PresenceTripwireHidOps {
    std::function<int()> init =
        []() noexcept { return hid_init(); };
    std::function<hid_device*(const char*)> open_path =
        [](const char* p) noexcept { return hid_open_path(p); };
    std::function<int(hid_device*, uint8_t*, size_t, int)> read_timeout =
        [](hid_device* d, uint8_t* b, size_t s, int t) noexcept {
            return hid_read_timeout(d, b, s, t);
        };
    std::function<void(hid_device*)> close_fn =
        [](hid_device* d) noexcept { if (d) hid_close(d); };
    std::function<void()> exit_fn =
        []() noexcept { hid_exit(); };
};

class PresenceTripwire {
public:
    using PresenceCallback = std::function<void(bool present, int confidence)>;
    using HidOps = PresenceTripwireHidOps;

    explicit PresenceTripwire(const ISensorFactory& factory, HidOps hid_ops = {});
    ~PresenceTripwire() noexcept;

    PresenceTripwire(const PresenceTripwire&) = delete;
    PresenceTripwire& operator=(const PresenceTripwire&) = delete;
    PresenceTripwire(PresenceTripwire&&) = delete;
    PresenceTripwire& operator=(PresenceTripwire&&) = delete;

    [[nodiscard]] bool start(const std::string& hidraw_node,
                             const HardwareId& hardware_id,
                             const PresenceCallback& cb) noexcept;
    void stop() noexcept;

private:
    void poll_loop() noexcept;

    const ISensorFactory& factory_;
    HidOps hid_ops_;           // declared before handle_ — initialised first
    PresenceCallback callback_;
    std::unique_ptr<SensorParser> parser_;
    std::atomic<bool> running_{false};
    ScopedWorker worker_;
    using HidDevicePtr = std::unique_ptr<hid_device, std::function<void(hid_device*)>>;
    HidDevicePtr handle_;
};
