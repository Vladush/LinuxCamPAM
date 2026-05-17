#pragma once
#include "constants.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>

struct PamConfig {
  uid_t min_uid = linuxcampam::DEFAULT_MIN_UID;
#ifndef DISABLE_WELCOME_MESSAGE
  bool show_welcome = true;
  std::string welcome_message = "LinuxCamPAM: Welcome, %u!";
#endif
};

// Section name → { key → value }. Last write wins within each section.
using IniData = std::map<std::string, std::map<std::string, std::string>>;

struct PamConfigState {
  std::string current_section;
  IniData data;
};

inline std::string_view trim(std::string_view s) {
  auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos) {
    return {};
  }
  auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

inline void process_pam_config_line(std::string_view line,
                                    PamConfigState &state) {
  std::string_view sv = trim(line);
  if (sv.empty() || sv[0] == ';' || sv[0] == '#') {
    return;
  }

  if (sv[0] == '[' && sv.back() == ']') {
    state.current_section = std::string(sv.substr(1, sv.size() - 2));
    return;
  }

  auto eq_pos = sv.find('=');
  if (eq_pos == std::string_view::npos) {
    return;
  }

  std::string_view key = trim(sv.substr(0, eq_pos));
  std::string_view val = trim(sv.substr(eq_pos + 1));

  if (!key.empty()) {
    state.data[state.current_section][std::string(key)] = std::string(val);
  }
}

inline PamConfig resolve_pam_config(const PamConfigState &state) {
  PamConfig config;

  // Lookup: [Security] > any other section that defines the key > default.
  auto get_value = [&](const std::string &key) -> std::string {
    auto sec_it = state.data.find("Security");
    if (sec_it != state.data.end()) {
      auto kv_it = sec_it->second.find(key);
      if (kv_it != sec_it->second.end()) {
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

    return {};
  };

  // --- min_uid ---
  std::string uid_str = get_value("min_uid");
  if (!uid_str.empty() && uid_str[0] != '-') {
    char *end = nullptr;
    constexpr int BASE_DECIMAL = 10;
    unsigned long parsed = std::strtoul(uid_str.c_str(), &end, BASE_DECIMAL);
    if (end != uid_str.c_str()) {
      config.min_uid = static_cast<uid_t>(parsed);
    }
  }

#ifndef DISABLE_WELCOME_MESSAGE
  // --- show_welcome ---
  std::string sw_str = get_value("show_welcome");
  if (!sw_str.empty()) {
    config.show_welcome = (sw_str == "true" || sw_str == "1" || sw_str == "yes");
  }

  // --- welcome_message ---
  std::string wm_str = get_value("welcome_message");
  if (!wm_str.empty()) {
    if (wm_str.size() >= 2 && wm_str.front() == '"' && wm_str.back() == '"') {
      wm_str = wm_str.substr(1, wm_str.size() - 2);
    }
    config.welcome_message = std::move(wm_str);
  }
#endif

  return config;
}

// C-style FILE* avoids iostream, which causes linker issues in PIC PAM modules.
inline PamConfig load_pam_config(const char *path) {
  PamConfigState state;

  struct FileCloser {
    void operator()(FILE *f) const {
      if (f) {
        (void)std::fclose(f); // NOLINT
      }
    }
  };

  std::unique_ptr<FILE, FileCloser> f(std::fopen(path, "r")); // NOLINT

  if (!f) {
    return resolve_pam_config(state);
  }

  constexpr size_t CONFIG_BUF_SIZE = 1024;
  std::array<char, CONFIG_BUF_SIZE> buffer{};

  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), f.get())) {
    process_pam_config_line(buffer.data(), state);
  }

  return resolve_pam_config(state);
}
