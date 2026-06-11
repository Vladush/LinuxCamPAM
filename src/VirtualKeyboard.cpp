#include "VirtualKeyboard.hpp"
#include "service/logger.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <utility>
#include <linux/uinput.h>

namespace {
constexpr uint16_t VIRTUAL_KEYBOARD_VENDOR_ID = 0x1234;
constexpr uint16_t VIRTUAL_KEYBOARD_PRODUCT_ID = 0x5678;

struct UniqueFd {
    int fd = -1;
    explicit UniqueFd(int f) : fd(f) {}
    ~UniqueFd() { if (fd >= 0) close(fd); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            if (fd >= 0) close(fd);
            fd = std::exchange(other.fd, -1);
        }
        return *this;
    }
    int get() const { return fd; }
    int release() { return std::exchange(fd, -1); }
};
} // namespace

VirtualKeyboard::VirtualKeyboard() : fd_(-1) {}

VirtualKeyboard::~VirtualKeyboard() {
    if (fd_ >= 0) {
        ioctl(fd_, UI_DEV_DESTROY);
        close(fd_);
        fd_ = -1;
    }
}

bool VirtualKeyboard::init() {
    if (fd_ >= 0) return true;

    UniqueFd temp_fd(open("/dev/uinput", O_WRONLY | O_NONBLOCK));
    if (temp_fd.get() < 0) {
        log_error("Failed to open /dev/uinput for VirtualKeyboard");
        return false;
    }

    // Enable key events
    if (ioctl(temp_fd.get(), UI_SET_EVBIT, EV_KEY) < 0) {
        log_error("Failed to set EV_KEY on /dev/uinput");
        return false;
    }

    // Enable the specific wake up key
    if (ioctl(temp_fd.get(), UI_SET_KEYBIT, KEY_WAKEUP) < 0) {
        log_error("Failed to set KEY_WAKEUP on /dev/uinput");
        return false;
    }

    struct uinput_setup usetup = {};
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor  = VIRTUAL_KEYBOARD_VENDOR_ID;
    usetup.id.product = VIRTUAL_KEYBOARD_PRODUCT_ID;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    std::strncpy(usetup.name, "LinuxCamPAM Virtual Keyboard", UINPUT_MAX_NAME_SIZE - 1);

    if (ioctl(temp_fd.get(), UI_DEV_SETUP, &usetup) < 0) {
        log_error("Failed to setup uinput device");
        return false;
    }

    if (ioctl(temp_fd.get(), UI_DEV_CREATE) < 0) {
        log_error("Failed to create uinput device");
        return false;
    }

    fd_ = temp_fd.release();
    log_info("VirtualKeyboard successfully initialized");
    return true;
}

bool VirtualKeyboard::emit_wakeup() {
    if (fd_ < 0) {
        if (!init()) return false;
    }

    struct input_event ev = {};

    // Press
    ev.type = EV_KEY;
    ev.code = KEY_WAKEUP;
    ev.value = 1;
    if (write(fd_, &ev, sizeof(ev)) < 0) {
        log_error("Failed to write KEY_WAKEUP press event");
        return false;
    }

    // Release
    ev = {};
    ev.type = EV_KEY;
    ev.code = KEY_WAKEUP;
    ev.value = 0;
    if (write(fd_, &ev, sizeof(ev)) < 0) {
        log_error("Failed to write KEY_WAKEUP release event");
        return false;
    }

    // Sync
    ev = {};
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    if (write(fd_, &ev, sizeof(ev)) < 0) {
        log_error("Failed to write SYN_REPORT event");
        return false;
    }

    log_debug("Emitted KEY_WAKEUP via VirtualKeyboard");
    return true;
}
