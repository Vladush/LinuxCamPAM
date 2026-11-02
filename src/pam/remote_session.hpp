#pragma once

#include <algorithm>
#include <array>
#include <string_view>

namespace linuxcampam::pam {

// PAM services that only ever authenticate users who are not at the machine.
inline constexpr std::array<std::string_view, 4> REMOTE_SERVICES = {
    "sshd", "remote", "cockpit", "xrdp-sesman"};

// PAM_RHOST values that still mean "this machine". sudo can set PAM_RHOST to
// the local hostname, so that counts as local too.
[[nodiscard]] inline bool is_local_rhost(std::string_view rhost,
                                         std::string_view hostname) {
  constexpr std::array<std::string_view, 6> loopback = {
      "localhost", "localhost.localdomain", "127.0.0.1",
      "::1",       "::ffff:127.0.0.1",      "0:0:0:0:0:0:0:1"};
  if (rhost.empty()) return true;
  if (std::find(loopback.begin(), loopback.end(), rhost) != loopback.end())
    return true;
  return !hostname.empty() && rhost == hostname;
}

// The camera can only see someone sitting at this machine, so a match during a
// remote login proves nothing about the remote party. Treat the session as
// remote if the service is remote-only or PAM_RHOST names another host.
[[nodiscard]] inline bool is_remote_session(std::string_view service,
                                            std::string_view rhost,
                                            std::string_view hostname) {
  if (std::find(REMOTE_SERVICES.begin(), REMOTE_SERVICES.end(), service) !=
      REMOTE_SERVICES.end())
    return true;
  return !is_local_rhost(rhost, hostname);
}

} // namespace linuxcampam::pam
