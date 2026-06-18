#include "utils.hpp"

#include "constants.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <linux/videodev2.h>
#include <string_view>
#include <sys/ioctl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <pwd.h>

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
extern char **environ;

namespace fs = std::filesystem;

namespace linuxcampam {

// --- RealCameraBackend Implementation ---

std::vector<std::string> RealCameraBackend::getDevicePaths() const {
  std::vector<std::string> paths;
  if (!fs::exists("/dev"))
    return paths;

  for (const auto &entry : fs::directory_iterator("/dev")) {
    std::string name = entry.path().filename().string();
    if (name.rfind("video", 0) == 0) {
      paths.push_back(entry.path().string());
    }
  }
  return paths;
}

bool RealCameraBackend::isVideoCaptureDevice(std::string_view path) const {
  FileDescriptor fd(open(std::string(path).c_str(), O_RDONLY | O_NONBLOCK));
  if (!fd.isValid())
    return false;

  struct v4l2_capability cap = {};
  if (ioctl(fd.get(), VIDIOC_QUERYCAP, &cap) < 0)
    return false;
  return (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE);
}

std::vector<uint32_t>
RealCameraBackend::getPixelFormats(std::string_view path) const {
  std::vector<uint32_t> formats;
  FileDescriptor fd(open(std::string(path).c_str(), O_RDONLY | O_NONBLOCK));
  if (!fd.isValid())
    return formats;

  struct v4l2_fmtdesc fmt = {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  while (ioctl(fd.get(), VIDIOC_ENUM_FMT, &fmt) == 0) {
    formats.push_back(fmt.pixelformat);
    fmt.index++;
  }
  return formats;
}

// --- Core Logic with Dependency Injection ---

std::string classifyCameraType(std::string_view device_path,
                               const ICameraBackend &backend) {
  if (!backend.isVideoCaptureDevice(device_path)) {
    return "";
  }

  bool has_grey = false;
  bool has_color = false;
  auto formats = backend.getPixelFormats(device_path);

  for (auto fmt : formats) {
    if (fmt == V4L2_PIX_FMT_GREY || fmt == V4L2_PIX_FMT_Y10 ||
        fmt == V4L2_PIX_FMT_Y12 || fmt == V4L2_PIX_FMT_Y16)
      has_grey = true;
    if (fmt == V4L2_PIX_FMT_MJPEG || fmt == V4L2_PIX_FMT_YUYV ||
        fmt == V4L2_PIX_FMT_RGB24 || fmt == V4L2_PIX_FMT_BGR24)
      has_color = true;
  }

  if (has_grey && !has_color)
    return "ir";
  if (has_color)
    return "rgb";
  if (has_grey)
    return "ir";
  return "generic";
}

std::vector<std::pair<std::string, std::string>>
enumerateCameras(const ICameraBackend &backend) {
  std::vector<std::pair<std::string, std::string>> cameras;
  auto paths = backend.getDevicePaths();

  for (const auto &path : paths) {
    std::string type = classifyCameraType(path, backend);
    if (!type.empty()) {
      cameras.emplace_back(path, type);
    }
  }
  std::sort(cameras.begin(), cameras.end());
  return cameras;
}

// --- Default Overloads ---

std::string classifyCameraType(std::string_view device_path) {
  RealCameraBackend backend;
  return classifyCameraType(device_path, backend);
}

std::vector<std::pair<std::string, std::string>> enumerateCameras() {
  RealCameraBackend backend;
  return enumerateCameras(backend);
}

int poll_remaining_ms(std::chrono::steady_clock::time_point deadline,
                      std::chrono::steady_clock::time_point now) {
  if (now >= deadline)
    return 0;
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
          .count();
  if (remaining > INT_MAX)
    return INT_MAX;
  return static_cast<int>(remaining);
}

std::string getIREmitterVersion(std::string_view path) {
  if (path.empty()) return {};

  auto is_safe = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c))
        || c == '/' || c == '.' || c == '_' || c == '-';
  };
  if (!std::all_of(path.begin(), path.end(), is_safe)) return {};
  if (!fs::exists(path)) return {};

  std::array<int, 2> pipefd{};
  if (pipe(pipefd.data()) != 0) return {};

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);

  std::string path_str(path);
  std::string flag = "-V";
  std::array<char*, 3> argv = {
    path_str.data(),
    flag.data(),
    nullptr
  };

  pid_t pid = 0;
  int status = posix_spawn(&pid, path_str.c_str(), &actions, nullptr,
                           argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pipefd[1]);

  std::string version;
  if (status == 0) {
    constexpr size_t buf_size = 128;
    std::array<char, buf_size> buf{};
    ssize_t n = read(pipefd[0], buf.data(), buf.size() - 1);
    if (n > 0) {
      version.assign(buf.data(), static_cast<size_t>(n));
      while (!version.empty() && (version.back() == '\n' || version.back() == '\r'))
        version.pop_back();
    }
    waitpid(pid, nullptr, 0);
  }
  close(pipefd[0]);
  return version;
}

int execute_command_spawn(const std::string& command_line) {
  constexpr size_t MAX_COMMAND_LENGTH = 4096;
  if (command_line.empty() || command_line.length() > MAX_COMMAND_LENGTH) {
    return -1;
  }

  // Simple string tokenizer respecting single and double quotes
  std::vector<std::string> args;
  std::string current_arg;
  bool in_single_quote = false;
  bool in_double_quote = false;
  bool escape_next = false;

  for (char c : command_line) {
    // Basic sanitization: reject unprintable control characters (except whitespace)
    if (std::iscntrl(static_cast<unsigned char>(c)) && !std::isspace(static_cast<unsigned char>(c))) {
      return -1;
    }

    if (escape_next) {
      current_arg += c;
      escape_next = false;
      continue;
    }
    if (c == '\\') {
      escape_next = true;
      continue;
    }
    if (c == '\'' && !in_double_quote) {
      in_single_quote = !in_single_quote;
      continue;
    }
    if (c == '"' && !in_single_quote) {
      in_double_quote = !in_double_quote;
      continue;
    }
    if (std::isspace(c) && !in_single_quote && !in_double_quote) {
      if (!current_arg.empty()) {
        args.push_back(current_arg);
        current_arg.clear();
      }
      continue;
    }
    current_arg += c;
  }
  if (!current_arg.empty()) {
    args.push_back(current_arg);
  }

  if (args.empty()) return -1;

  struct ArgvGuard {
    std::vector<char*> ptrs;
    ~ArgvGuard() { std::for_each(ptrs.begin(), ptrs.end(), free); }
    ArgvGuard() = default;
    ArgvGuard(const ArgvGuard&) = delete;
    ArgvGuard& operator=(const ArgvGuard&) = delete;
    ArgvGuard(ArgvGuard&&) = delete;
    ArgvGuard& operator=(ArgvGuard&&) = delete;
  } argv;

  argv.ptrs.reserve(args.size() + 1);
  std::transform(args.begin(), args.end(), std::back_inserter(argv.ptrs),
                 [](const std::string& arg) { return strdup(arg.c_str()); });
  argv.ptrs.push_back(nullptr);

  pid_t pid = -1;
  int status = posix_spawnp(&pid, argv.ptrs[0], nullptr, nullptr, argv.ptrs.data(), environ);

  if (status != 0) {
    return status;
  }

  int wait_status = 0;
  if (waitpid(pid, &wait_status, 0) != -1) {
    if (WIFEXITED(wait_status)) {
      return WEXITSTATUS(wait_status);
    }
  }
  return -1;
}

bool isValidUsername(std::string_view username) {
  if (username.empty() || username.length() > linuxcampam::MAX_USERNAME_LENGTH)
    return false;

  // Hidden files
  if (username.front() == '.')
    return false;

  // Detect Path Traversal ("..")
  // Using explicit loop or adjacent_find
  for (size_t i = 1; i < username.length(); ++i) {
    if (username[i] == '.' && username[i - 1] == '.')
      return false;
  }

  // Strict allowlist (a-z, A-Z, 0-9, _, ., -, $)
  return std::all_of(username.begin(), username.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' ||
           c == '$';
  });
}

std::optional<std::filesystem::path> getHomeDir(uid_t uid) {
  struct passwd pwd{};
  struct passwd *result = nullptr;
  constexpr size_t GETPWUID_BUFFER_SIZE = 16384;
  std::vector<char> buf(GETPWUID_BUFFER_SIZE);
  if (getpwuid_r(uid, &pwd, buf.data(), buf.size(), &result) == 0 && result) {
    return std::filesystem::path(result->pw_dir);
  }
  return std::nullopt;
}

std::optional<uid_t> getUidForUsername(std::string_view username) {
  struct passwd pwd{};
  struct passwd *result = nullptr;
  constexpr size_t GETPWNAM_BUFFER_SIZE = 16384;
  std::vector<char> buf(GETPWNAM_BUFFER_SIZE);
  // pw_name is null-terminated, so we must make a safe string first
  std::string uname(username);
  if (getpwnam_r(uname.c_str(), &pwd, buf.data(), buf.size(), &result) == 0 && result) {
    return result->pw_uid;
  }
  return std::nullopt;
}

} // namespace linuxcampam
