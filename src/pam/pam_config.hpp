#pragma once
#include "constants.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

// Basic INI parser for min_uid
struct PamConfig {
  uid_t min_uid = linuxcampam::DEFAULT_MIN_UID;
};

// Internal parsing state
struct PamConfigState {
  std::string current_section;
  uid_t found_security_uid = 0;
  bool has_security_uid = false;
  uid_t found_first_uid = 0;
  bool has_first_uid = false;
};

// Utilities
inline bool starts_with(std::string_view s, std::string_view prefix) {
  return s.substr(0, prefix.size()) == prefix;
}

inline std::string_view trim(std::string_view s) {
  s.remove_prefix(std::min(s.find_first_not_of(" \t\r\n"), s.size()));
  s.remove_suffix(
      std::min(s.size() - s.find_last_not_of(" \t\r\n") - 1, s.size()));
  return s;
}

// Logic only - no IO
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

  // Key=Value parsing
  size_t eq_pos = sv.find('=');
  if (eq_pos != std::string_view::npos) {
    std::string_view key = trim(sv.substr(0, eq_pos));
    std::string_view val = trim(sv.substr(eq_pos + 1));

    if (key == "min_uid") {
      if (starts_with(val, "-")) {
        return;
      }

      // Use C99 strtoul because C++17 std::from_chars causes linker errors with
      // -fPIC
      char *end = nullptr;
      std::string val_str(val);
      constexpr int BASE_DECIMAL = 10;
      unsigned long parsed = std::strtoul(val_str.c_str(), &end, BASE_DECIMAL);

      if (end != val_str.c_str()) {
        uid_t parsed_uid = static_cast<uid_t>(parsed);
        bool is_security = (state.current_section == "Security");

        if (is_security) {
          state.found_security_uid = parsed_uid;
          state.has_security_uid = true;
        } else if (!state.has_first_uid && !state.has_security_uid) {
          // Capture the first valid setting we find, unless we already have
          // one. We'll prioritize [Security] later if it exists.
          if (!state.has_first_uid) {
            state.found_first_uid = parsed_uid;
            state.has_first_uid = true;
          }
        }
      }
    }
  }
}

inline PamConfig resolve_pam_config(const PamConfigState &state) {
  PamConfig config;
  // Priority Logic: [Security] > First Found > Default
  if (state.has_security_uid) {
    config.min_uid = state.found_security_uid;
  } else if (state.has_first_uid) {
    config.min_uid = state.found_first_uid;
  }
  return config;
}

// C-style FILE* is ugly but effective here. avoiding iostream keeps the PAM
// module linker happy (especially with PIC).
inline PamConfig load_pam_config(const char *path) {
  PamConfigState state;

  // Simple RAII to auto-close the file.
  struct FileCloser {
    void operator()(FILE *f) const {
      if (f) {
        (void)std::fclose(f); // NOLINT
      }
    }
  };

  // Grab the file handle and wrap it up immediately.
  std::unique_ptr<FILE, FileCloser> f(std::fopen(path, "r")); // NOLINT

  if (!f) {
    // No config file - defaults work just fine.
    return resolve_pam_config(state);
  }

  constexpr size_t CONFIG_BUF_SIZE = 1024;
  std::array<char, CONFIG_BUF_SIZE> buffer{};

  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), f.get())) {
    process_pam_config_line(buffer.data(), state);
  }

  return resolve_pam_config(state);
}
