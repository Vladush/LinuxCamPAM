#pragma once

#include <string>
#include <chrono>
#include <optional>
#include <string_view>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <filesystem>

class HardwareId {
    static constexpr size_t MAX_HARDWARE_ID_LENGTH = 32;
    std::string id_;
public:
    explicit HardwareId(std::string_view id) {
        if (id.empty() || id.size() > MAX_HARDWARE_ID_LENGTH || 
            !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c); })) {
            throw std::invalid_argument("Invalid Hardware ID");
        }
        id_ = id;
    }
    std::string_view get() const noexcept { return id_; }
    const std::string& str() const noexcept { return id_; }
};

class HardwareManager {
private:
    static constexpr int MAX_HIDRAW_RETRIES = 5; // Maximum number of attempts to find the hidraw node
    static constexpr auto UDEV_LATENCY_DELAY = std::chrono::milliseconds(100); // Wait time between attempts to find the hidraw node

    std::string i2c_address;
    std::string hid_id;
    std::string previous_hid_driver;

    [[nodiscard]] static bool write_sysfs(const std::string& path, std::string_view value);
    [[nodiscard]] bool resolve_hid_device();
    void resolve_current_driver();
    std::filesystem::path sys_i2c_path_;
    std::filesystem::path sys_hid_path_;
    std::filesystem::path dev_path_;

public:
    explicit HardwareManager(
        std::string addr,
        std::filesystem::path sys_i2c_path = "/sys/bus/i2c/devices",
        std::filesystem::path sys_hid_path = "/sys/bus/hid/devices",
        std::filesystem::path dev_path = "/dev"
    );

    HardwareManager(const HardwareManager&) = delete;
    HardwareManager& operator=(const HardwareManager&) = delete;
    HardwareManager(HardwareManager&&) = delete;
    HardwareManager& operator=(HardwareManager&&) = delete;

    ~HardwareManager() noexcept;

    [[nodiscard]] bool seize_sensor();
    void release_sensor();

    [[nodiscard]] std::optional<std::string> get_hidraw_node() const;
};
