#include <security/pam_modules.h>
#include <security/pam_ext.h>
#include <cstring>

extern "C" {
PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int, int,
                                   const char **) {
  const void *tok = nullptr;
  int ret = pam_get_authtok(pamh, PAM_AUTHTOK, reinterpret_cast<const char **>(&tok), "Password: ");
  if (ret != PAM_SUCCESS) {
    return ret;
  }
  if (tok != nullptr && std::strcmp(static_cast<const char *>(tok), "secret") == 0) {
    return PAM_SUCCESS;
  }
  return PAM_AUTH_ERR;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *, int, int, const char **) {
  return PAM_IGNORE;
}
}
