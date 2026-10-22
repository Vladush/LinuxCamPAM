#pragma once

#include <cstdio>
#include <memory>
#include <unistd.h>
#include <utility>

namespace linuxcampam {
struct FileCloser {
  void operator()(FILE *file) const noexcept {
    if (file) (void)std::fclose(file);
  }
};
using FileHandle = std::unique_ptr<FILE, FileCloser>;

class FileDescriptor {
  int fd_ = -1;
public:
  FileDescriptor() noexcept = default;
  explicit FileDescriptor(int fd) noexcept : fd_(fd) {}
  ~FileDescriptor() { if (fd_ >= 0) close(fd_); }
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  FileDescriptor(FileDescriptor &&other) noexcept : fd_(other.release()) {}
  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) close(fd_);
      fd_ = other.release();
    }
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool isValid() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
  operator int() const noexcept { return fd_; }
};
} // namespace linuxcampam
