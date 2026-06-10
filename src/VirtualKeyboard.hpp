#pragma once


class VirtualKeyboard {
public:
    VirtualKeyboard();
    ~VirtualKeyboard();

    // Disable copy/move
    VirtualKeyboard(const VirtualKeyboard&) = delete;
    VirtualKeyboard& operator=(const VirtualKeyboard&) = delete;
    VirtualKeyboard(VirtualKeyboard&&) = delete;
    VirtualKeyboard& operator=(VirtualKeyboard&&) = delete;

    // Initializes the /dev/uinput device
    [[nodiscard]] bool init();

    // Emits a KEY_WAKEUP event
    [[nodiscard]] bool emit_wakeup();

private:
    int fd_;
    bool initialized_;
};
