#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>

namespace linuxcampam {

namespace fs = std::filesystem;

inline constexpr size_t kMaxCameraPathLength = 256;

struct CameraPathInfo {
  std::string canonical_path;
  int device_id = -1;
};

// Check if string matches /dev/video[0-9]+
[[nodiscard]] inline bool isStandardVideoDeviceString(std::string_view str) {
  constexpr std::string_view prefix = "/dev/video";
  if (str.rfind(prefix, 0) != 0 || str.size() <= prefix.size()) {
    return false;
  }
  return std::all_of(str.begin() + prefix.size(), str.end(), [](unsigned char c) {
    return std::isdigit(c) != 0;
  });
}

// Extract the integer index from /dev/video[0-9]+
[[nodiscard]] inline std::optional<int> extractVideoDeviceId(std::string_view video_path) {
  if (!isStandardVideoDeviceString(video_path)) {
    return std::nullopt;
  }
  constexpr std::string_view prefix = "/dev/video";
  std::string suffix(video_path.substr(prefix.size()));
  try {
    return std::stoi(suffix);
  } catch (...) {
    return std::nullopt;
  }
}

// Validates camera device paths without requiring hardware presence.
// Accepts /dev/video[0-9]+ and persistent udev symlink prefixes.
[[nodiscard]] inline bool isValidCameraPath(std::string_view path) {
  if (path.empty() || path.size() > kMaxCameraPathLength) {
    return false;
  }
  if (path.find("..") != std::string_view::npos ||
      path.find('\0') != std::string_view::npos ||
      path.find(';') != std::string_view::npos ||
      path.find('|') != std::string_view::npos ||
      path.find('&') != std::string_view::npos) {
    return false;
  }

  if (isStandardVideoDeviceString(path)) {
    return true;
  }

  constexpr std::string_view by_id_prefix = "/dev/v4l/by-id/";
  constexpr std::string_view by_path_prefix = "/dev/v4l/by-path/";

  auto is_valid_udev_leaf = [](std::string_view leaf) {
    if (leaf.empty()) {
      return false;
    }
    return std::all_of(leaf.begin(), leaf.end(), [](char c) {
      unsigned char uc = static_cast<unsigned char>(c);
      return (std::isalnum(uc) != 0) || c == '.' || c == '_' || c == ':' || c == '-';
    });
  };

  if (path.rfind(by_id_prefix, 0) == 0) {
    return is_valid_udev_leaf(path.substr(by_id_prefix.size()));
  }
  if (path.rfind(by_path_prefix, 0) == 0) {
    return is_valid_udev_leaf(path.substr(by_path_prefix.size()));
  }

  return false;
}

// Resolves a camera path (canonicalizing symlinks if present and existing).
// If require_char_device is true, verifies that the target node exists and is S_ISCHR.
[[nodiscard]] inline std::optional<CameraPathInfo> resolveCameraPath(
    std::string_view raw_path, bool require_char_device = false) {
  if (!isValidCameraPath(raw_path)) {
    return std::nullopt;
  }

  std::error_code ec;
  fs::path p(raw_path);

  // If path exists on filesystem, canonicalize symlinks
  if (fs::exists(p, ec)) {
    fs::path canonical = fs::canonical(p, ec);
    if (ec) {
      return std::nullopt;
    }

    struct stat st {};
    if (stat(canonical.c_str(), &st) != 0) {
      return std::nullopt;
    }

    if (require_char_device && !S_ISCHR(st.st_mode)) {
      return std::nullopt;
    }

    std::string canon_str = canonical.string();
    if (!isStandardVideoDeviceString(canon_str)) {
      return std::nullopt;
    }

    auto id = extractVideoDeviceId(canon_str);
    if (!id) {
      return std::nullopt;
    }

    return CameraPathInfo{canon_str, *id};
  }

  // Path does not exist in filesystem (e.g. unit test or offline validation)
  if (require_char_device) {
    return std::nullopt;
  }

  if (isStandardVideoDeviceString(raw_path)) {
    auto id = extractVideoDeviceId(raw_path);
    if (id) {
      return CameraPathInfo{std::string(raw_path), *id};
    }
  }

  return std::nullopt;
}

} // namespace linuxcampam
