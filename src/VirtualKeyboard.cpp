#include "VirtualKeyboard.hpp"
#include "service/logger.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <linux/uinput.h>

namespace {
constexpr uint16_t VIRTUAL_KEYBOARD_VENDOR_ID = 0x1234;
constexpr uint16_t VIRTUAL_KEYBOARD_PRODUCT_ID = 0x5678;
} // namespace

VirtualKeyboard::VirtualKeyboard() : fd_(-1), initialized_(false) {}

VirtualKeyboard::~VirtualKeyboard() {
    if (fd_ >= 0) {
        ioctl(fd_, UI_DEV_DESTROY);
        close(fd_);
        fd_ = -1;
    }
}

bool VirtualKeyboard::init() {
    if (initialized_) return true;

    fd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd_ < 0) {
        log_error("Failed to open /dev/uinput for VirtualKeyboard");
        return false;
    }

    // Enable key events
    if (ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0) {
        log_error("Failed to set EV_KEY on /dev/uinput");
        close(fd_);
        fd_ = -1;
        return false;
    }

    // Enable the specific wake up key
    if (ioctl(fd_, UI_SET_KEYBIT, KEY_WAKEUP) < 0) {
        log_error("Failed to set KEY_WAKEUP on /dev/uinput");
        close(fd_);
        fd_ = -1;
        return false;
    }

    struct uinput_setup usetup = {};
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor  = VIRTUAL_KEYBOARD_VENDOR_ID;
    usetup.id.product = VIRTUAL_KEYBOARD_PRODUCT_ID;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    std::strncpy(usetup.name, "LinuxCamPAM Virtual Keyboard", UINPUT_MAX_NAME_SIZE - 1);

    if (ioctl(fd_, UI_DEV_SETUP, &usetup) < 0) {
        log_error("Failed to setup uinput device");
        close(fd_);
        fd_ = -1;
        return false;
    }

    if (ioctl(fd_, UI_DEV_CREATE) < 0) {
        log_error("Failed to create uinput device");
        close(fd_);
        fd_ = -1;
        return false;
    }

    initialized_ = true;
    log_info("VirtualKeyboard successfully initialized");
    return true;
}

bool VirtualKeyboard::emit_wakeup() {
    if (!initialized_) {
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
