---
title: PAM_LINUXCAMPAM
section: 8
header: LinuxCamPAM System Manager's Manual
footer: LinuxCamPAM 0.9.7.5
date: June 2026
---

<!-- markdownlint-disable MD025 -->

# NAME

pam_linuxcampam - PAM module for face authentication

# SYNOPSIS

`pam_linuxcampam.so`

# DESCRIPTION

**pam_linuxcampam** is a Pluggable Authentication Module (PAM) that authenticates users via face recognition.

It communicates with the `linuxcampamd` service to perform the actual biometric verification. If the service is unreachable or authentication fails, it returns `PAM_AUTH_ERR`, typically allowing the PAM stack to fall back to password authentication (if configured as `sufficient`).

# MODULE ARGUMENTS

**no_welcome**
:   Suppresses the welcome message usually displayed upon successful authentication. This overrides any configuration set in `/etc/linuxcampam/config.ini`.

All other configuration is handled via `/etc/linuxcampam/config.ini` to ensure security and centralization.

# KDE LOCK SCREEN

When the PAM service is `kde` and the configuration is set to `single_enter` (or `auto` resolves to it), this module injects an empty string into `PAM_AUTHTOK` upon successful face authentication if the token is previously unset. This is designed to satisfy `pam_kwallet5.so` and bypass its password prompt, allowing for a single-Enter unlock experience. 

In `auto` mode, the module determines compatibility by scanning `/etc/pam.d/kde` and `/usr/lib/pam.d/kde` (in that order) to check if an active authentication rule for `pam_kwallet5.so` or `pam_kwallet6.so` is present.

# FILES

*/etc/pam.d/common-auth*
:   Typical location for enabling this module.

# SEE ALSO

**pam.conf**(5), **pam.d**(5), **linuxcampamd**(8), **linuxcampam.conf**(5)
