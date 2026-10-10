#pragma once

#include <string>
#include <string_view>
#include <map>
#include <optional>
#include <charconv>
#include <limits>
#include <stdexcept>

namespace config_parser {

using IniData = std::map<std::string, std::map<std::string, std::string>>;

inline std::string_view trim(std::string_view s) {
  auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos) return {};
  auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

inline std::string_view unquote(std::string_view s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

inline std::string_view strip_comments_and_trim(std::string_view sv) {
  bool in_quotes = false;
  size_t end_idx = sv.size();
  for (size_t i = 0; i < sv.size(); ++i) {
    if (sv[i] == '"') {
      in_quotes = !in_quotes;
    } else if (!in_quotes && (sv[i] == ';' || sv[i] == '#')) {
      end_idx = i;
      break;
    }
  }
  return trim(sv.substr(0, end_idx));
}

inline void parse_ini_line(std::string_view line, std::string& current_section, IniData& data) {
  std::string_view sv = strip_comments_and_trim(line);
  if (sv.empty()) return;

  if (sv.front() == '[' && sv.back() == ']') {
    current_section = std::string(trim(sv.substr(1, sv.size() - 2)));
    return;
  }

  auto eq_pos = sv.find('=');
  if (eq_pos == std::string_view::npos) return;

  std::string_view key = trim(sv.substr(0, eq_pos));
  std::string_view val = trim(sv.substr(eq_pos + 1));

  if (!key.empty()) {
    data[current_section][std::string(key)] = std::string(val);
  }
}

inline std::optional<bool> parse_bool_strict(std::string_view val) {
  auto unq = unquote(val);
  if (unq == "true" || unq == "1" || unq == "on" || unq == "yes") return true;
  if (unq == "false" || unq == "0" || unq == "off" || unq == "no") return false;
  return std::nullopt;
}

inline std::optional<int> parse_int_strict(std::string_view val, int min_val = std::numeric_limits<int>::min(), int max_val = std::numeric_limits<int>::max()) {
  auto unq = trim(val);
  if (unq.empty()) return std::nullopt;
  int parsed = 0;
  auto [ptr, ec] = std::from_chars(unq.data(), unq.data() + unq.size(), parsed);
  if (ec == std::errc{} && ptr == unq.data() + unq.size()) {
    if (parsed >= min_val && parsed <= max_val) return parsed;
  }
  return std::nullopt;
}

inline std::optional<float> parse_float_strict(std::string_view val, float min_val = -std::numeric_limits<float>::infinity(), float max_val = std::numeric_limits<float>::infinity()) {
  auto unq = trim(val);
  if (unq.empty()) return std::nullopt;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
  float parsed = 0.0f;
  auto [ptr, ec] = std::from_chars(unq.data(), unq.data() + unq.size(), parsed);
  if (ec == std::errc{} && ptr == unq.data() + unq.size()) {
    if (parsed >= min_val && parsed <= max_val) return parsed;
  }
  return std::nullopt;
#else
  try {
    size_t pos = 0;
    float parsed = std::stof(std::string(unq), &pos);
    if (pos == unq.size() && parsed >= min_val && parsed <= max_val) return parsed;
  } catch (...) {}
  return std::nullopt;
#endif
}

} // namespace config_parser
