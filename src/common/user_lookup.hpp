#pragma once

#include <cerrno>
#include <optional>
#include <pwd.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace linuxcampam {
inline std::optional<uid_t> getUidForUsername(std::string_view username) {
  const std::string name(username);
  const long suggested = sysconf(_SC_GETPW_R_SIZE_MAX);
  constexpr size_t MAX_BUFFER = 1024 * 1024;
  size_t size = suggested > 0 ? static_cast<size_t>(suggested) : 16384;
  for (; size <= MAX_BUFFER; size *= 2) {
    std::vector<char> buffer(size);
    passwd entry{};
    passwd *result = nullptr;
    const int error = getpwnam_r(name.c_str(), &entry, buffer.data(), buffer.size(), &result);
    if (error == 0) return result ? std::optional<uid_t>(entry.pw_uid) : std::nullopt;
    if (error != ERANGE) break;
  }
  return std::nullopt;
}
} // namespace linuxcampam
