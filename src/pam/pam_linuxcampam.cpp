#include "constants.hpp"
#include "ipc_protocol.hpp"
#include "pam_config.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <pwd.h>
#include <security/pam_ext.h>
#include <security/pam_modules.h>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>
#include <utility>

namespace {
constexpr size_t BUFFER_SIZE = 128;
constexpr int TIMEOUT_SEC = 5;

struct SocketDescriptor {
  int fd = -1;
  explicit SocketDescriptor(int f) : fd(f) {}
  ~SocketDescriptor() {
    if (fd >= 0) {
      close(fd);
    }
  }
  SocketDescriptor(const SocketDescriptor &) = delete;
  SocketDescriptor &operator=(const SocketDescriptor &) = delete;
  SocketDescriptor(SocketDescriptor &&other) noexcept : fd(std::exchange(other.fd, -1)) {}
  SocketDescriptor &operator=(SocketDescriptor &&other) noexcept {
    if (this != &other) {
      if (fd >= 0) {
        close(fd);
      }
      fd = std::exchange(other.fd, -1);
    }
    return *this;
  }
  [[nodiscard]] int get() const { return fd; }
  [[nodiscard]] bool isValid() const { return fd >= 0; }
};

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

struct PamResponseDeleter {
  void operator()(struct pam_response *resp) const {
    if (resp) {
      if (resp[0].resp) {
        size_t len = std::strlen(resp[0].resp);
        volatile char *p = resp[0].resp;
        while (len--) {
          *p++ = 0;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
        free(resp[0].resp);
      }
      // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
      free(resp);
    }
  }
};
using PamResponsePtr = std::unique_ptr<struct pam_response, PamResponseDeleter>;

// RAII handles openlog/closelog automatically
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static SyslogManager syslog_manager;
} // namespace

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
PAM_EXTERN int pam_sm_setcred([[maybe_unused]] pam_handle_t *pamh,
                              [[maybe_unused]] int flags,
                              [[maybe_unused]] int argc,
                              [[maybe_unused]] const char **argv) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  return PAM_SUCCESS;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
PAM_EXTERN int pam_sm_acct_mgmt([[maybe_unused]] pam_handle_t *pamh,
                                [[maybe_unused]] int flags,
                                [[maybe_unused]] int argc,
                                [[maybe_unused]] const char **argv) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  return PAM_SUCCESS;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh,
                                   [[maybe_unused]] int flags,
                                   [[maybe_unused]] int argc,
                                   [[maybe_unused]] const char **argv)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  try {
    const char *user = nullptr;
    int retval = pam_get_user(pamh, &user, nullptr);
    if (retval != PAM_SUCCESS) {
      return retval;
    }

    PamConfig config = load_pam_config(linuxcampam::CONFIG_PATH);

#ifndef DISABLE_WELCOME_MESSAGE
    if (std::any_of(argv, argv + argc, [](const char *arg) {
          return arg != nullptr && std::strcmp(arg, "no_welcome") == 0;
        })) {
      config.show_welcome = false;
    }
#endif

    struct passwd pwd = {};
    struct passwd *result = nullptr;
    constexpr long DEFAULT_PW_BUF_SIZE = 16384;
    long bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufsize == -1) bufsize = DEFAULT_PW_BUF_SIZE;
    std::vector<char> buffer(bufsize);

    if (getpwnam_r(user, &pwd, buffer.data(), buffer.size(), &result) == 0 && result != nullptr) {
      // If min_uid is 0, the check is disabled.
      // Otherwise, skip authentication for any user with UID < min_uid.
      if (config.min_uid > 0 && result->pw_uid < config.min_uid) {
        syslog(LOG_INFO, "Skipping auth for system user: %s (UID %d < %d)",
               user, result->pw_uid, config.min_uid);
        return PAM_IGNORE;
      }
    } else {
      // User doesn't exist locally (or NSS failed).
      // Since we can't verify the UID, we have to assume they aren't allowed.
      syslog(LOG_WARNING, "User not found in system database: %s", user);
      return PAM_USER_UNKNOWN;
    }
    // Create a socket to the authentication service
    if (flags & PAM_SILENT) {
      // If Silent, just debug log the version.
      syslog(LOG_DEBUG, "pam_linuxcampam version %s", LINUXCAMPAM_VERSION);
    } else {
      // Otherwise, log the version.
      syslog(LOG_INFO, "pam_linuxcampam version %s", LINUXCAMPAM_VERSION);
    }

    if (config.require_confirmation) {
      const void *service_ptr = nullptr;
      if (pam_get_item(pamh, PAM_SERVICE, &service_ptr) == PAM_SUCCESS && service_ptr != nullptr) {
        const char *service = static_cast<const char *>(service_ptr);
        std::string_view sv_service{service};
        auto it = std::find(config.confirmation_exempt_services.begin(),
                            config.confirmation_exempt_services.end(),
                            sv_service);
        if (it == config.confirmation_exempt_services.end()) {
          const char* msg_text = "Press <Enter> to authenticate with face, or type password:";
          struct pam_message msg = {
              .msg_style = PAM_PROMPT_ECHO_OFF,
              .msg = msg_text
          };
          const struct pam_message *msgp = &msg;
          struct pam_response *resp_pam = nullptr;

          const void *conv_ptr = nullptr;
          if (int ret = pam_get_item(pamh, PAM_CONV, &conv_ptr); 
              ret == PAM_SUCCESS && conv_ptr != nullptr) {
            const struct pam_conv *conv = static_cast<const struct pam_conv *>(conv_ptr);
            int conv_ret = conv->conv(1, &msgp, &resp_pam, conv->appdata_ptr);
            PamResponsePtr resp_ptr(resp_pam);
            if (conv_ret != PAM_SUCCESS) {
              syslog(LOG_ERR, "Authentication confirmation failed or canceled for service: %s", service);
              return PAM_AUTH_ERR;
            }
            if (resp_pam && resp_pam[0].resp && std::strlen(resp_pam[0].resp) > 0) {
              syslog(LOG_INFO, "User provided password input, skipping face auth to allow fallback.");
              pam_set_item(pamh, PAM_AUTHTOK, resp_pam[0].resp);
              return PAM_IGNORE;
            }
          } else {
            syslog(LOG_ERR, "Failed to get PAM_CONV for confirmation");
            return PAM_AUTHINFO_UNAVAIL;
          }
        }
      }
    }

    SocketDescriptor sock(socket(AF_UNIX, SOCK_STREAM, 0));
    if (!sock.isValid()) {
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
    setsockopt(sock.get(), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&tv), sizeof tv);
    setsockopt(sock.get(), SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char *>(&tv), sizeof tv);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    if (connect(sock.get(), reinterpret_cast<struct sockaddr *>(&addr),
                sizeof(addr)) == -1) {
      // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
      syslog(LOG_INFO, "Could not connect to linuxcampamd socket - service may "
                       "not be running");
      return PAM_AUTHINFO_UNAVAIL;
    }

    // Use protocol to serialize request
    linuxcampam::protocol::Request req{
        linuxcampam::protocol::Command::AUTH_REQUEST, {user}};
    std::string reqStr = req.serialize();

    if (send(sock.get(), reqStr.c_str(), reqStr.length(), 0) < 0) {
      syslog(LOG_ERR, "Failed to send auth request: %m");
      return PAM_AUTHINFO_UNAVAIL;
    }

    std::array<char, BUFFER_SIZE> response_buffer = {};
    ssize_t valread = read(sock.get(), response_buffer.data(), response_buffer.size() - 1);

    if (valread > 0) {
      std::string resp(response_buffer.data(), static_cast<size_t>(valread));
      if (resp.find("AUTH_SUCCESS") != std::string::npos) {
#ifndef DISABLE_WELCOME_MESSAGE
        if (config.show_welcome && ((flags & PAM_SILENT) == 0)) {
          std::string welcome_msg = config.welcome_message;
          for (size_t pos = welcome_msg.find("%u");
               pos != std::string::npos;
               pos = welcome_msg.find("%u", pos + std::strlen(user))) {
            welcome_msg.replace(pos, 2, user);
          }

          if (!welcome_msg.empty()) {
            struct pam_message msg = {
                .msg_style = PAM_TEXT_INFO,
                .msg = welcome_msg.c_str()
            };
            const struct pam_message *msgp = &msg;
            struct pam_response *resp_pam = nullptr;

            const void *conv_ptr = nullptr;
            int ret = pam_get_item(pamh, PAM_CONV, &conv_ptr);
            if (ret == PAM_SUCCESS && conv_ptr != nullptr) {
              const struct pam_conv *conv = static_cast<const struct pam_conv *>(conv_ptr);
              conv->conv(1, &msgp, &resp_pam, conv->appdata_ptr);
              PamResponsePtr resp_ptr(resp_pam);
            }
          }
        }
#endif // DISABLE_WELCOME_MESSAGE

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
