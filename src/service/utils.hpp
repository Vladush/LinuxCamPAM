#pragma once

#include <string>
#include <unistd.h>
#include <utility>
#include <vector>
namespace linuxcampam {

struct FileDescriptor {
  int fd = -1;

  explicit FileDescriptor(int f) : fd(f) {}
  ~FileDescriptor() {
    if (fd >= 0) {
      close(fd);
    }
  }

  // Disable copy
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;

  // Allow move
  FileDescriptor(FileDescriptor &&other) noexcept : fd(other.fd) {
    other.fd = -1;
  }
  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      if (fd >= 0) {
        close(fd);
      }
      fd = other.fd;
      other.fd = -1;
    }
    return *this;
  }

  [[nodiscard]] int get() const { return fd; }
  [[nodiscard]] bool isValid() const { return fd >= 0; }
  operator int() const { return fd; }
};

// Camera Utilities
[[nodiscard]] std::string classifyCameraType(const std::string &device_path);
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
enumerateCameras();

} // namespace linuxcampam