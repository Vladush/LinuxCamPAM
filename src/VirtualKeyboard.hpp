#pragma once

#include <mutex>

class VirtualKeyboard {
public:
    VirtualKeyboard() = default;
    ~VirtualKeyboard();

    VirtualKeyboard(const VirtualKeyboard&) = delete;
    VirtualKeyboard& operator=(const VirtualKeyboard&) = delete;
    VirtualKeyboard(VirtualKeyboard&&) = delete;
    VirtualKeyboard& operator=(VirtualKeyboard&&) = delete;

    [[nodiscard]] bool init();
    [[nodiscard]] bool emit_wakeup();

private:
    [[nodiscard]] bool init_locked(); // requires fd_mtx_ held by caller

    std::mutex fd_mtx_;
    int uinput_fd_{-1};
};
