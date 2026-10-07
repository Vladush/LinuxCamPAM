#include <security/pam_modules.h>
#include <security/pam_ext.h>
#include <cstdlib>

extern "C" {
PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int, int,
                                   const char **) {
  const void *tok = nullptr;
  pam_get_item(pamh, PAM_AUTHTOK, &tok);
  if (tok == nullptr) {
    pam_putenv(pamh, "FAKE_KWALLET=prompted");
    char *resp = nullptr;
    pam_prompt(pamh, PAM_PROMPT_ECHO_OFF, &resp, "Password: ");
    free(resp);
    return PAM_IGNORE;
  }
  pam_putenv(pamh, *static_cast<const char *>(tok) == '\0'
                       ? "FAKE_KWALLET=empty"
                       : "FAKE_KWALLET=password");
  return PAM_IGNORE;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *, int, int, const char **) {
  return PAM_IGNORE;
}
}
