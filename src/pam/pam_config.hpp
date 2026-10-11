#pragma once
#include "constants.hpp"
#include "../common/resources.hpp"
#include "../common/config_parser.hpp"

#include <algorithm>
#include <cerrno>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class KdeLockscreenMode : std::uint8_t { Auto, SingleEnter, Legacy };

struct PamConfig {
  int auth_timeout_ms = linuxcampam::DEFAULT_AUTH_TIMEOUT_MS;
  uid_t min_uid = linuxcampam::DEFAULT_MIN_UID;
  bool require_confirmation = true;
  std::vector<std::string> confirmation_exempt_services = {
      "gdm-password", "sddm", "lightdm", "login", 
      "swaylock", "i3lock", "xscreensaver", "kscreenlocker", "kde", "systemd-user"
  };
  bool exempt_services_explicit = false;
  KdeLockscreenMode kde_lockscreen = KdeLockscreenMode::Auto;
  bool kde_lockscreen_invalid = false;
#ifndef DISABLE_WELCOME_MESSAGE
  bool show_welcome = true;
  std::string welcome_message = "LinuxCamPAM: Welcome, %u!";
#endif
};

struct PamConfigState {
  std::string current_section;
  config_parser::IniData data;
};

[[nodiscard]] inline std::optional<KdeLockscreenMode>
parse_kde_lockscreen(std::string_view value) {
  value = config_parser::trim(config_parser::unquote(config_parser::trim(value)));
  if (value == "auto") return KdeLockscreenMode::Auto;
  if (value == "single_enter") return KdeLockscreenMode::SingleEnter;
  if (value == "legacy") return KdeLockscreenMode::Legacy;
  return std::nullopt;
}

[[nodiscard]] inline const char *to_string(KdeLockscreenMode mode) {
  switch (mode) {
  case KdeLockscreenMode::Auto: return "auto";
  case KdeLockscreenMode::SingleEnter: return "single_enter";
  case KdeLockscreenMode::Legacy: return "legacy";
  }
  return "legacy";
}

inline void process_pam_config_line(std::string_view line,
                                    PamConfigState &state) {
  config_parser::parse_ini_line(line, state.current_section, state.data);
}

[[nodiscard]] inline PamConfig resolve_pam_config(const PamConfigState &state) {
  PamConfig config;

  // Lookup: [Security] > any other section that defines the key > default.
  auto get_value = [&](const std::string &key) -> std::optional<std::string> {
    if (auto sec_it = state.data.find("Security"); sec_it != state.data.end()) {
      if (auto kv_it = sec_it->second.find(key); kv_it != sec_it->second.end()) {
        return kv_it->second;
      }
    }

    auto it = std::find_if(
        state.data.begin(), state.data.end(),
        [&](const auto &pair) {
          return pair.first != "Security" &&
                 pair.second.find(key) != pair.second.end();
        });
    if (it != state.data.end()) {
      return it->second.at(key);
    }

    return std::nullopt;
  };

  // Match daemon precedence for the authentication deadline.
  for (const char *section : {"Auth", "General"}) {
    auto it = state.data.find(section);
    if (it == state.data.end()) continue;
    auto timeout = it->second.find("timeout_ms");
    if (timeout == it->second.end()) continue;
    auto value = config_parser::parse_int_strict(timeout->second, 1, linuxcampam::MAX_AUTH_TIMEOUT_MS);
    if (!value) throw std::invalid_argument("Invalid authentication timeout");
    config.auth_timeout_ms = *value;
    break;
  }

  // --- min_uid ---
  if (auto uid_opt = get_value("min_uid")) {
    if (auto parsed = config_parser::parse_int_strict(*uid_opt, 0)) {
      config.min_uid = static_cast<uid_t>(*parsed);
    }
  }

  // --- require_confirmation ---
  if (auto rc_opt = get_value("require_confirmation")) {
    if (auto b = config_parser::parse_bool_strict(*rc_opt)) {
      config.require_confirmation = *b;
    }
  }

  // --- confirmation_exempt_services ---
  if (auto ces_opt = get_value("confirmation_exempt_services")) {
    config.confirmation_exempt_services = config_parser::split_trimmed(*ces_opt, ',');
    config.exempt_services_explicit = true;
  }

  // --- kde_lockscreen ---
  if (auto kl_opt = get_value("kde_lockscreen")) {
    if (auto mode = parse_kde_lockscreen(*kl_opt)) {
      config.kde_lockscreen = *mode;
    } else {
      config.kde_lockscreen = KdeLockscreenMode::Legacy;
      config.kde_lockscreen_invalid = true;
    }
  }

#ifndef DISABLE_WELCOME_MESSAGE
  // --- show_welcome ---
  if (auto sw_opt = get_value("show_welcome")) {
    if (auto b = config_parser::parse_bool_strict(*sw_opt)) {
      config.show_welcome = *b;
    }
  }

  // --- welcome_message ---
  if (auto wm_opt = get_value("welcome_message")) {
    config.welcome_message = *wm_opt;
  }
#endif

  return config;
}

// C-style FILE* avoids iostream, which causes linker issues in PIC PAM modules.
[[nodiscard]] inline PamConfig load_pam_config(const char *path) {
  PamConfigState state;

  linuxcampam::FileHandle f(std::fopen(path, "re"));

  if (!f) {
    if (errno != ENOENT) throw std::invalid_argument("Cannot open PAM configuration");
    return resolve_pam_config(state);
  }

  // Accumulate complete lines so long exemption lists are never parsed in pieces.
  std::string line;
  for (int c = std::fgetc(f.get()); c != EOF; c = std::fgetc(f.get())) {
    if (c == '\n') {
      process_pam_config_line(line, state);
      line.clear();
    } else {
      if (line.size() >= 64 * 1024) throw std::invalid_argument("Configuration line too long");
      line += static_cast<char>(c);
    }
  }
  if (std::ferror(f.get())) throw std::invalid_argument("Cannot read PAM configuration");
  if (!line.empty()) process_pam_config_line(line, state);

  return resolve_pam_config(state);
}
