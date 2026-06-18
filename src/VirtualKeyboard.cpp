#include "VirtualKeyboard.hpp"
#include "service/logger.hpp"
#include "service/utils.hpp"

#include <cstring>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
constexpr uint16_t VIRTUAL_KEYBOARD_VENDOR_ID  = 0x1234;
constexpr uint16_t VIRTUAL_KEYBOARD_PRODUCT_ID = 0x5678;
} // namespace

VirtualKeyboard::~VirtualKeyboard() {
    std::lock_guard<std::mutex> lock(fd_mtx_);
    if (uinput_fd_ >= 0) {
        ioctl(uinput_fd_, UI_DEV_DESTROY);
        close(uinput_fd_);
        uinput_fd_ = -1;
    }
}

bool VirtualKeyboard::init_locked() {
    if (uinput_fd_ >= 0) return true;

    linuxcampam::FileDescriptor temp_fd(open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC));
    if (!temp_fd.isValid()) {
        log_error("Failed to open /dev/uinput for VirtualKeyboard");
        return false;
    }

    if (ioctl(temp_fd.get(), UI_SET_EVBIT, EV_KEY) < 0) {
        log_error("Failed to set EV_KEY on /dev/uinput");
        return false;
    }

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

    // Transfer fd ownership
    uinput_fd_ = temp_fd.release();
    log_info("VirtualKeyboard successfully initialized");
    return true;
}

bool VirtualKeyboard::init() {
    std::lock_guard<std::mutex> lock(fd_mtx_);
    return init_locked();
}

bool VirtualKeyboard::emit_wakeup() {
    std::lock_guard<std::mutex> lock(fd_mtx_);
    if (!init_locked()) return false;

    struct input_event ev = {};

    ev.type  = EV_KEY;
    ev.code  = KEY_WAKEUP;
    ev.value = 1;
    if (write(uinput_fd_, &ev, sizeof(ev)) < 0) {
        log_error("Failed to write KEY_WAKEUP press event");
        return false;
    }

    ev       = {};
    ev.type  = EV_KEY;
    ev.code  = KEY_WAKEUP;
    ev.value = 0;
    if (write(uinput_fd_, &ev, sizeof(ev)) < 0) {
        log_error("Failed to write KEY_WAKEUP release event");
        return false;
    }

    ev       = {};
    ev.type  = EV_SYN;
    ev.code  = SYN_REPORT;
    ev.value = 0;
    if (write(uinput_fd_, &ev, sizeof(ev)) < 0) {
        log_error("Failed to write SYN_REPORT event");
        return false;
    }

    log_debug("Emitted KEY_WAKEUP via VirtualKeyboard");
    return true;
}
