#pragma once
#include "../common/resources.hpp"
#include "../common/user_lookup.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>
#include <filesystem>
#include <optional>
namespace linuxcampam {

// Camera Utilities

// Abstract interface for camera device interaction
class ICameraBackend {
public:
  ICameraBackend() = default;
  virtual ~ICameraBackend() = default;

  ICameraBackend(const ICameraBackend &) = delete;
  ICameraBackend &operator=(const ICameraBackend &) = delete;
  ICameraBackend(ICameraBackend &&) = delete;
  ICameraBackend &operator=(ICameraBackend &&) = delete;

  // Returns a list of paths to potential camera devices (e.g., /dev/video0)
  [[nodiscard]] virtual std::vector<std::string> getDevicePaths() const = 0;

  // Returns true if the device at path supports video capture
  [[nodiscard]] virtual bool
  isVideoCaptureDevice(std::string_view path) const = 0;

  // Returns a list of supported pixel formats (V4L2_PIX_FMT_*) for the device
  [[nodiscard]] virtual std::vector<uint32_t>
  getPixelFormats(std::string_view path) const = 0;
};

// Concrete implementation using real V4L2 calls
class RealCameraBackend : public ICameraBackend {
public:
  [[nodiscard]] std::vector<std::string> getDevicePaths() const override;
  [[nodiscard]] bool
  isVideoCaptureDevice(std::string_view path) const override;
  [[nodiscard]] std::vector<uint32_t>
  getPixelFormats(std::string_view path) const override;
};

// Core logic with dependency injection
[[nodiscard]] std::string classifyCameraType(std::string_view device_path,
                                             const ICameraBackend &backend);
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
enumerateCameras(const ICameraBackend &backend);

// Default wrappers using RealCameraBackend
[[nodiscard]] std::string classifyCameraType(std::string_view device_path);
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
enumerateCameras();

// Helpers
[[nodiscard]] std::string getIREmitterVersion(std::string_view path);

// Computes ms remaining until deadline.
// Clamps to [0, INT_MAX] to prevent infinite poll() blocks.
[[nodiscard]] int
poll_remaining_ms(std::chrono::steady_clock::time_point deadline,
                  std::chrono::steady_clock::time_point now);

// Execute a shell-free command using posix_spawnp
[[nodiscard]] int execute_command_spawn(const std::string& command_line);

// Security Utils
[[nodiscard]] bool isValidUsername(std::string_view username);
[[nodiscard]] std::optional<std::filesystem::path> getHomeDir(uid_t uid);

} // namespace linuxcampam