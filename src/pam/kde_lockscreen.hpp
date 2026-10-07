#pragma once
#include "pam_config.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <algorithm>
#include <cstdint>

namespace linuxcampam::kde {

inline constexpr std::string_view SERVICE = "kde";

inline constexpr size_t MAX_STACK_BYTES = 64ULL * 1024ULL;

#ifdef LINUXCAMPAM_TEST_PAM_DIR
inline constexpr const char *ETC_STACK = LINUXCAMPAM_TEST_PAM_DIR "/etc/kde";
inline constexpr const char *VENDOR_STACK =
    LINUXCAMPAM_TEST_PAM_DIR "/vendor/kde";
#else
inline constexpr const char *ETC_STACK = "/etc/pam.d/kde";
inline constexpr const char *VENDOR_STACK = "/usr/lib/pam.d/kde";
#endif

enum class Line : std::uint8_t { Other, KwalletAuth, OtherKwalletAuth, Unsupported };
enum class Scan : std::uint8_t {
  KwalletAuth, NoKwalletAuth, OtherKwalletAuth,
  Missing, Unreadable, TooLarge, Unsupported
};

struct StackInfo {
  const char *path = nullptr;
  Scan scan = Scan::Missing;
};

struct Decision {
  bool single_enter = false;
  const char *reason = "not the kde service";
};

namespace detail {
[[nodiscard]] inline std::pair<std::string_view, std::string_view>
next_token(std::string_view s) {
  s = trim(s);
  auto end = s.find_first_of(" \t");
  if (end == std::string_view::npos) {
    return {s, {}};
  }
  return {s.substr(0, end), s.substr(end)};
}
} // namespace detail

[[nodiscard]] inline Line classify_line(std::string_view line) {
  if (auto hash = line.find('#'); hash != std::string_view::npos) {
    line = line.substr(0, hash);
  }
  line = trim(line);
  if (line.empty() || line.front() == '@') {
    return Line::Other;
  }
  if (line.back() == '\\') {
    return Line::Unsupported;
  }

  auto [type, rest] = detail::next_token(line);
  if (!type.empty() && type.front() == '-') {
    type.remove_prefix(1);
  }
  if (type != "auth") {
    return Line::Other;
  }

  rest = trim(rest);
  if (!rest.empty() && rest.front() == '[') {
    auto close = rest.find(']');
    if (close == std::string_view::npos) {
      return Line::Unsupported;
    }
    rest = rest.substr(close + 1);
  } else {
    rest = detail::next_token(rest).second;
  }

  auto module = detail::next_token(rest).first;
  if (auto slash = module.rfind('/'); slash != std::string_view::npos) {
    module.remove_prefix(slash + 1);
  }
  if (module == "pam_kwallet5.so" || module == "pam_kwallet6.so") {
    return Line::KwalletAuth;
  }
  constexpr std::string_view kwallet_prefix = "pam_kwallet";
  if (module.size() >= kwallet_prefix.size() && module.substr(0, kwallet_prefix.size()) == kwallet_prefix) {
    return Line::OtherKwalletAuth;
  }
  return Line::Other;
}

[[nodiscard]] inline Scan scan_text(std::string_view text) {
  bool kwallet = false;
  bool other = false;
  while (!text.empty()) {
    auto nl = text.find('\n');
    auto line = text.substr(0, nl);
    text = nl == std::string_view::npos ? std::string_view{}
                                        : text.substr(nl + 1);
    switch (classify_line(line)) {
    case Line::KwalletAuth: kwallet = true; break;
    case Line::OtherKwalletAuth: other = true; break;
    case Line::Unsupported: return Scan::Unsupported;
    case Line::Other: break;
    }
  }
  if (kwallet) return Scan::KwalletAuth;
  return other ? Scan::OtherKwalletAuth : Scan::NoKwalletAuth;
}

[[nodiscard]] inline Scan scan_file(const char *path) {
  struct FileCloser {
    void operator()(FILE *f) const {
      if (f) {
        (void)std::fclose(f); // NOLINT
      }
    }
  };
  std::unique_ptr<FILE, FileCloser> f(std::fopen(path, "re")); // NOLINT
  if (!f) {
    return errno == ENOENT ? Scan::Missing : Scan::Unreadable;
  }
  if (std::fseek(f.get(), 0, SEEK_END) != 0) {
    return Scan::Unreadable;
  }
  long size = std::ftell(f.get());
  if (size < 0) {
    return Scan::Unreadable;
  }
  if (static_cast<size_t>(size) > MAX_STACK_BYTES) {
    return Scan::TooLarge;
  }
  if (std::fseek(f.get(), 0, SEEK_SET) != 0) {
    return Scan::Unreadable;
  }

  std::string text;
  text.resize(static_cast<size_t>(size));
  size_t n = std::fread(text.data(), 1, text.size(), f.get());
  if (n != text.size() && std::ferror(f.get())) {
    return Scan::Unreadable;
  }
  text.resize(n);
  return scan_text(text);
}

[[nodiscard]] inline StackInfo scan_effective_stack() {
  Scan etc = scan_file(ETC_STACK);
  if (etc != Scan::Missing) {
    return {ETC_STACK, etc};
  }
  return {VENDOR_STACK, scan_file(VENDOR_STACK)};
}

[[nodiscard]] inline Decision decide(const PamConfig &config, const StackInfo &stack) {
  switch (config.kde_lockscreen) {
  case KdeLockscreenMode::Legacy:
    return {false, config.kde_lockscreen_invalid
                       ? "invalid kde_lockscreen value"
                       : "kde_lockscreen = legacy"};
  case KdeLockscreenMode::SingleEnter:
    return {true, "kde_lockscreen = single_enter"};
  case KdeLockscreenMode::Auto:
    break;
  }
  switch (stack.scan) {
  case Scan::KwalletAuth: return {true, "KWallet auth rule found"};
  case Scan::NoKwalletAuth: return {false, "no KWallet auth rule"};
  case Scan::OtherKwalletAuth: return {false, "unsupported KWallet module"};
  case Scan::Missing: return {false, "kde PAM file not found"};
  case Scan::Unreadable: return {false, "kde PAM file unreadable"};
  case Scan::TooLarge: return {false, "kde PAM file too large"};
  case Scan::Unsupported: return {false, "unsupported PAM syntax"};
  }
  return {false, "unknown"};
}

[[nodiscard]] inline bool is_confirmation_exempt(const PamConfig &config,
                                   std::string_view service,
                                   const Decision &kde) {
  if (kde.single_enter && service == SERVICE &&
      !config.exempt_services_explicit) {
    return false;
  }
  const auto &list = config.confirmation_exempt_services;
  return std::find(list.begin(), list.end(), service) != list.end();
}

} // namespace linuxcampam::kde
