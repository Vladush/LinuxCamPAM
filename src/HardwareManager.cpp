#include "HardwareManager.hpp"
#include <system_error>
#include <filesystem>
#include <fstream>
#include <thread>

bool HardwareManager::write_sysfs(const std::string& path, std::string_view value) {
    if (std::ofstream file{path}) {
        file << value;
        file.close(); // Force flush
        return !file.fail(); // Check for any stream errors
    }
    return false;
}

bool HardwareManager::resolve_hid_device() {
    auto path = sys_i2c_path_ / i2c_address;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
        constexpr std::string_view PREFIX = "0018:";
        std::string name = entry.path().filename().string();
        if (name.rfind(PREFIX, 0) == 0) {
            hid_id = std::move(name);
            return true;
        }
    }
    return false;
}

void HardwareManager::resolve_current_driver() {
    auto driver_link = sys_hid_path_ / hid_id / "driver";
    std::error_code ec;
    if (std::filesystem::exists(driver_link, ec) && std::filesystem::is_symlink(driver_link, ec)) {
        auto target = std::filesystem::read_symlink(driver_link, ec);
        if (!ec) {
            previous_hid_driver = target.filename().string();
        } else {
            previous_hid_driver.clear();
        }
    } else {
        previous_hid_driver.clear();
    }
}

HardwareManager::HardwareManager(std::string addr, std::filesystem::path sys_i2c_path, std::filesystem::path sys_hid_path, std::filesystem::path dev_path) 
    : i2c_address(std::move(addr)), sys_i2c_path_(std::move(sys_i2c_path)), sys_hid_path_(std::move(sys_hid_path)), dev_path_(std::move(dev_path)) {}

HardwareManager::~HardwareManager() noexcept {
    try {
        release_sensor();
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // Suppress any exceptions during destruction
    }
}

bool HardwareManager::seize_sensor() {
    if (!write_sysfs((sys_i2c_path_ / i2c_address / "power" / "wakeup").string(), "disabled")) {
        return false;
    }
    if (!resolve_hid_device()) return false;
    resolve_current_driver();
    
    // The existing driver (e.g., hid-sensor-hub) is NOT unbound and hid-generic is not forced.
    // hid-generic rejects sensor hubs, and hidraw supports concurrent access natively.
    // The driver currently bound to the device will continue providing the hidraw node.

    return true;
}

void HardwareManager::release_sensor() {
    if (hid_id.empty()) return;
    // Drivers are no longer unbound/bound, so just restore power/wakeup state
    (void)write_sysfs((sys_i2c_path_ / i2c_address / "power" / "wakeup").string(), "enabled");
}

std::optional<std::string> HardwareManager::get_hidraw_node() const {
    if (hid_id.empty()) return std::nullopt;
    auto parent_path = sys_hid_path_ / hid_id;
    auto search_path = parent_path / "hidraw";
    std::error_code ec;

    if (!std::filesystem::exists(parent_path, ec)) {
        return std::nullopt;
    }

    for (int i = 0; i < MAX_HIDRAW_RETRIES; ++i) { // Allow udev latency mapping
        if (std::filesystem::exists(search_path, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(search_path, ec)) {
                return (dev_path_ / entry.path().filename()).string();
            }
        }
        std::this_thread::sleep_for(UDEV_LATENCY_DELAY);
    }
    return std::nullopt;
}
