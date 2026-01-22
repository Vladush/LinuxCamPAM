#include "constants.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <security/pam_ext.h>
#include <security/pam_modules.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr size_t BUFFER_SIZE = 128;
constexpr int TIMEOUT_SEC = 5;
// Duplicate removed

struct SyslogManager {
  SyslogManager() {
    openlog("LinuxCamPAM", LOG_PID | LOG_NDELAY, LOG_AUTHPRIV);
  }
  ~SyslogManager() { closelog(); }
  SyslogManager(const SyslogManager &) = delete;
  SyslogManager &operator=(const SyslogManager &) = delete;
  SyslogManager(SyslogManager &&) = delete;
  SyslogManager &operator=(SyslogManager &&) = delete;
};

// Manage syslog lifecycle.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static SyslogManager syslog_manager;
} // namespace

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc,
                              const char **argv) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  (void)pamh;
  (void)flags;
  (void)argc;
  (void)argv;
  return PAM_SUCCESS;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
PAM_EXTERN int pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc,
                                const char **argv) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  (void)pamh;
  (void)flags;
  (void)argc;
  (void)argv;
  return PAM_SUCCESS;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc,
                                   const char **argv)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  (void)flags;
  (void)argc;
  (void)argv;
  try {
    const char *user = nullptr;
    int retval = pam_get_user(pamh, &user, NULL);
    if (retval != PAM_SUCCESS) {
      return retval;
    }

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
      // Service unavailable or socket error -> Ignore and fallback to
      // password
      syslog(LOG_ERR, "Failed to create socket: %m");
      return PAM_AUTHINFO_UNAVAIL;
    }

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    (void)std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s",
                        linuxcampam::SOCKET_PATH);

    // Set timeout (exceeds detection timeout of 3s)
    // to avoid aborting while the camera is still looking.
    struct timeval tv = {};
    tv.tv_sec = TIMEOUT_SEC;
    tv.tv_usec = 0;
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&tv), sizeof tv);
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char *>(&tv), sizeof tv);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    if (connect(sock, reinterpret_cast<struct sockaddr *>(&addr),
                sizeof(addr)) == -1) {
      // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
      close(sock);
      // Service not running or unreachable -> Ignore
      syslog(LOG_INFO, "Could not connect to linuxcampamd socket - service may "
                       "not be running");
      return PAM_AUTHINFO_UNAVAIL;
    }

    std::string req = "AUTH_REQUEST " + std::string(user);
    if (send(sock, req.c_str(), req.length(), 0) < 0) {
      syslog(LOG_ERR, "Failed to send auth request: %m");
      close(sock);
      return PAM_AUTHINFO_UNAVAIL;
    }

    std::array<char, BUFFER_SIZE> buffer = {};
    ssize_t valread = read(sock, buffer.data(), buffer.size() - 1);
    close(sock);

    if (valread > 0) {
      std::string resp(buffer.data());
      if (resp.find("AUTH_SUCCESS") != std::string::npos) {
        // Display Welcome Message
        struct pam_message msg = {};
        const struct pam_message *msgp = nullptr;
        struct pam_response *resp_pam = nullptr;

        std::string welcome_msg =
            "LinuxCamPAM: Welcome, " + std::string(user) + "!";
        // PAM requires non-const char* but doesn't modify it in most
        // implementations However, standard says char*. We use const_cast as
        // we own the string. Create mutable buffer for PAM
        std::vector<char> msg_buf(welcome_msg.begin(), welcome_msg.end());
        msg_buf.push_back('\0');
        char *msg_cstr = msg_buf.data();

        msg.msg_style = PAM_TEXT_INFO;
        msg.msg = msg_cstr;
        msgp = &msg;

        const struct pam_conv *conv = nullptr;
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
        int ret = pam_get_item(pamh, PAM_CONV,
                               reinterpret_cast<const void **>(&conv));
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        if (ret == PAM_SUCCESS && conv != NULL) {
          // Best effort message, ignore return code
          conv->conv(1, &msgp, &resp_pam, conv->appdata_ptr);
          if (resp_pam) {
            std::unique_ptr<struct pam_response, decltype(&free)> resp_ptr(
                resp_pam, free);
          }
        }

        syslog(LOG_INFO, "Authentication successful for user: %s", user);
        return PAM_SUCCESS;
      } else {
        syslog(LOG_NOTICE, "Authentication failed for user: %s (Response: %s)",
               user, resp.c_str());
      }
    } else {
      syslog(LOG_ERR, "Failed to read response from service");
    }
  } catch (const std::exception &e) {
    syslog(LOG_ERR, "LinuxCamPAM: Exception during authentication: %s",
           e.what());
    return PAM_AUTHINFO_UNAVAIL;
  } catch (...) {
    syslog(LOG_ERR, "LinuxCamPAM: Unknown exception during authentication");
    return PAM_AUTHINFO_UNAVAIL;
  }
  return PAM_AUTH_ERR;
}
