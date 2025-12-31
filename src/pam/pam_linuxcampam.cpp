#include <cstring>
#include <security/pam_ext.h>
#include <security/pam_modules.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>

#include "constants.hpp"

PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc,
                              const char **argv) {
  return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc,
                                const char **argv) {
  return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc,
                                   const char **argv) {
  try {
    const char *user;
    int retval = pam_get_user(pamh, &user, NULL);
    if (retval != PAM_SUCCESS) {
      return retval;
    }

    // Connect to Service
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
      // Service unavailable or socket error -> Ignore and fallback to password
      return PAM_AUTHINFO_UNAVAIL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, linuxcampam::SOCKET_PATH, sizeof(addr.sun_path) - 1);

    // Set Timeout for connection
    // This is set to 5s to exceed the detection timeout (default 3s)
    // to avoid aborting while the camera is still looking.
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof tv);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
      close(sock);
      // Service not running or unreachable -> Ignore
      return PAM_AUTHINFO_UNAVAIL;
    }

    // Send Request
    std::string req = "AUTH_REQUEST " + std::string(user);
    if (send(sock, req.c_str(), req.length(), 0) < 0) {
      close(sock);
      return PAM_AUTHINFO_UNAVAIL;
    }

    // Read resp
    char buffer[128] = {0};
    int valread = read(sock, buffer, 127);
    close(sock);

    if (valread > 0) {
      std::string resp(buffer);
      if (resp.find("AUTH_SUCCESS") != std::string::npos) {
        // Display Welcome Message
        struct pam_message msg;
        const struct pam_message *msgp;
        struct pam_response *resp_pam = NULL;

        std::string welcome_msg =
            "LinuxCamPAM: Welcome, " + std::string(user) + "!";
        char *msg_cstr = const_cast<char *>(welcome_msg.c_str());

        msg.msg_style = PAM_TEXT_INFO;
        msg.msg = msg_cstr;
        msgp = &msg;

        const struct pam_conv *conv;
        int ret = pam_get_item(pamh, PAM_CONV, (const void **)&conv);
        if (ret == PAM_SUCCESS && conv != NULL) {
          // Best effort message, ignore return code
          conv->conv(1, &msgp, &resp_pam, conv->appdata_ptr);
          if (resp_pam)
            free(resp_pam);
        }

        return PAM_SUCCESS;
      }
    }

    // Auth failed or unexpected response -> PAM_AUTH_ERR (Ignored due to
    // config)
    return PAM_AUTH_ERR;

  } catch (...) {
    // Catch ALL exceptions (std::bad_alloc, logic errors) to prevent crashing
    // the host (gdm, sudo, login). Log to syslog could be added here if syslog
    // header is included, but safe fallback is priority.
    return PAM_AUTHINFO_UNAVAIL;
  }
}
