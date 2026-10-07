# 5. Empty Authtok for KDE Lock Screen

Date: 2026-10-08

## Status

Accepted

## Context

On KDE Plasma environments, users were prompted for a password twice when unlocking the screen. Specifically, after a successful face scan, the screen locker would display a prompt demanding a password. Pressing Enter (with an empty password) would then succeed.

The root cause is that KDE's lock screen PAM stack (`/usr/lib/pam.d/kde` or `/etc/pam.d/kde`) typically contains `auth optional pam_kwallet5.so`. The `pam_kwallet5.so` module prompts the user for a password if `PAM_AUTHTOK` is null/unset. If the token is empty (`""`), it ignores the prompt and gracefully continues. 

To provide a seamless "single-Enter" unlock experience without requiring users to modify their distribution-provided PAM files, we needed a way to satisfy this requirement safely.

## Decision

We decided to implement targeted empty token injection:
- We added a new configuration mode: `kde_lockscreen`.
- In `auto` mode (the default), LinuxCamPAM dynamically parses the `kde` PAM stack file. If an active `auth` rule for `pam_kwallet5.so` or `pam_kwallet6.so` is found, it resolves to `single_enter`.
- In `single_enter` mode, if the `PAM_SERVICE` is exactly `kde`, and the face authentication is successful, the LinuxCamPAM module manually sets `PAM_AUTHTOK` to an empty string (`""`) (only if it was previously unset).
- This triggers `pam_kwallet5.so` to skip its prompt.

Options considered and rejected:
- **PAM File Override**: Automatically rewriting `/etc/pam.d/kde` was rejected as too intrusive, prone to conflicts with OS updates, and a violation of the rule to not edit OS configurations dynamically.
- **`PAM_KWALLET5_LOGIN` Environment Shortcut**: Undocumented and potentially fragile.
- **Unconditional Token Injection**: Rejected because it could negatively impact other Desktop Environments (e.g., GNOME/GDM) where `pam_unix` might attempt to hash an empty password. 

## Consequences

* **Positive**: KDE users receive a significantly improved user experience (a single Enter to scan and unlock) out of the box, without needing to hack their system's PAM files.
* **Positive**: GNOME and other desktop environments remain entirely unaffected due to the strict `service == "kde"` check.
* **Neutral**: `pam_kwallet` does not actually unlock the user's wallet with an empty password, meaning KWallet will remain locked until the user types their real password elsewhere (this is identical to the behavior of fingerprint unlocking on KDE).
