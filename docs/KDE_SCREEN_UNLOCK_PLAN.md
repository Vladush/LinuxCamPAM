# KDE Screen Unlock Compatibility Plan

Status: Draft for review. Implementation has not started.

Date: 2026-10-07

Branch: `fix/kde-screen-unlock`

Base: local `origin/master`, commit `d560924d2426a7ca840867aa2427f837bbb33ce7`.

The branch has its own worktree. The uncommitted TPM/keyring work remains in
the original checkout on `feat/tpm-kwallet-unlock`.

## Objective

Support the behavior agreed during troubleshooting:

1. Show the KDE lock screen without starting a face scan.
2. Press Enter with the password field empty to start face authentication.
3. Unlock immediately after successful recognition, without another Enter.
4. Preserve ordinary password authentication and fallback after a failed scan.

Deliver this inside LinuxCamPAM, without editing distribution PAM files, and
keep existing installations working unchanged unless the new behavior is
detected as applicable or explicitly enabled.

This plan covers LinuxCamPAM's settings, PAM module behavior, a read-only
check, upgrade handling, tests, and documentation. The installed manual fix
has already been confirmed working by the user; this branch replaces it with
a reproducible, package-managed solution.

## Evidence and Existing Fix

The affected system runs Kubuntu 26.04, Plasma workspace 6.6.6,
libkscreenlocker6 6.6.5, libpam-kwallet5 6.6.4, Linux-PAM 1.7.0, and
LinuxCamPAM 0.9.7.5. SDDM is the display manager.

- Logs showed successful face authentication followed by
  `pam_kwallet5(kde:auth): Couldn't get password (it is empty)`.
- The second Enter completed that module's password conversation with an
  empty response. Recognition did not need to run again.
- The installed `LockScreenUi.qml` also distinguishes success after a PAM
  prompt from success without one through `authenticator.hadPrompt`.
  The latter displays a separate Unlock action.
- The vendor stack is `/usr/lib/pam.d/kde`, owned by `libkscreenlocker6`
  (line 6: `auth optional pam_kwallet5.so`). The package also ships
  `kde-fingerprint` and `kde-smartcard`. `/etc/pam.d/kde` is the manual
  override and belongs to no package.
- `common-auth` uses `[success=2 default=ignore] pam_linuxcampam.so`
  (generated from `[success=end default=ignore]` in our pam-auth-update
  profile). A successful scan jumps past `pam_unix`, and the KDE stack then
  continues with its own KWallet auth rule.
- Upstream `pam_kwallet.c` prompts for a password only when `PAM_AUTHTOK` is
  unset. An empty string is logged as "Empty or missing password, doing
  nothing" and returns `PAM_IGNORE` without a conversation.
- The working fix explicitly enables `require_confirmation`, removes only
  `kde` from `confirmation_exempt_services`, and disables the optional
  KWallet authentication rule in the `/etc/pam.d/kde` override.
- The configuration's immutable flag was temporarily cleared and restored.
- The apparent IR failure was a closed physical shutter. No emitter,
  driver, suspend-hook, or recognition-threshold change was needed.

Both configuration changes were tested together. Do not claim that each
individually resolves every variant of this symptom, or that every Plasma
6 installation has the same distribution-supplied PAM stack.

## Proposed Design

### 1. Lock-Screen Mode Setting

Add `kde_lockscreen` to the `[Security]` section:

| Value | Confirmation for `kde` | Empty token after face match |
| --- | --- | --- |
| `auto` (default) | as `single_enter` if the KDE stack has a KWallet auth rule, else as `legacy` | same rule |
| `single_enter` | required, unless an explicit exemption list says otherwise | yes |
| `legacy` | exempt via the built-in list (0.9.7.5 behavior) | no |

The setting affects only the `kde` service. Other services behave exactly as
before in every mode. An unknown value is logged and treated as `legacy`.

Explicit administrator settings keep precedence. A configured
`confirmation_exempt_services` is used as written, whatever the mode;
`require_confirmation = false` still disables the prompt everywhere. The mode
then only controls the token step.

The built-in exemption list in `pam_config.hpp` stays as it is, including
`kde`. `single_enter` removes `kde` from the effective list only when no
explicit list is configured.

### 2. Empty Token After a Face Match

After a successful scan for `kde` in `single_enter`, set `PAM_AUTHTOK` to
`""` if it is still unset. A typed password at the confirmation prompt is
already relayed through `PAM_AUTHTOK` and returns `PAM_IGNORE`; that path is
unchanged, so KWallet still receives a real password when one was entered.
Failed, cancelled, or unavailable scans never set the token.

The empty string is not a credential. Linux-PAM clears `PAM_AUTHTOK` at the
end of each `pam_authenticate()` call (`_pam_sanitize`), so the value cannot
leak into later attempts on the same handle; the integration tests check
this. After a successful jump only modules listed after `@include
common-auth` see it; on the tested stack that is `pam_kwallet5`.

If a future kwallet-pam release also prompts on an empty token, the result
is the current two-Enter behavior, not a lockout.

### 3. Auto Detection

`auto` is resolved inside the PAM module, only for the `kde` service:

1. Read `/etc/pam.d/kde`; if it does not exist, read `/usr/lib/pam.d/kde`.
   This matches Linux-PAM's own lookup order. Paths are fixed at build time.
2. Resolve to `single_enter` only if an active `auth` rule uses a module whose
   basename is exactly `pam_kwallet5.so`. Includes are not followed.
3. Resolve to `legacy` in every other case: missing or unreadable file, file
   above 64 KiB, unsupported syntax such as line continuations, or a
   different `pam_kwallet*` module name.

Expected results:

- A vendor Kubuntu 26.04 stack resolves to `single_enter`.
- The test laptop's manual override (KWallet auth rule removed) resolves to
  `legacy`. Its explicit exemption list still requires confirmation for
  `kde`, so it keeps working as it does today.
- Unknown or customized stacks keep the old behavior.

### 4. Read-Only Check and Optional Mode Writer

One implementation, `linuxcampam check-kde`, reuses the PAM module's headers
so the shell scripts never duplicate the parser. `linuxcampam-setup-config
--check-kde` delegates to it. It never writes files or probes cameras.

`linuxcampam-setup-config --configure-kde MODE` optionally changes only the
`kde_lockscreen` key, with a backup and immutable-flag handling. It never
edits PAM files.

Editing `/etc/pam.d/kde` remains a documented manual fallback for stacks that
auto detection does not cover. An existing override stays
administrator-managed: the check may say it is no longer required but never
removes or rewrites it.

### 5. Environmental Compatibility (GNOME, Other DEs/DMs)

A core requirement is that this solution works robustly alongside GNOME (Ubuntu 24.04/26.04), other Desktop Environments (DEs), Display Managers (DMs), and Window Managers.

The design ensures strict isolation for non-KDE environments:

1. **Targeted Service Matching**: The `kde_lockscreen` logic and empty token injection are strictly guarded by a service name check (`std::string_view{service} == "kde"`). If the active PAM service is `gdm-password` (GNOME), `sddm`, `lightdm`, `swaylock`, or `login`, the new behavior is completely bypassed.
2. **Exemption Preservation**: GNOME and other environments continue to rely on the `confirmation_exempt_services` list (which includes `gdm-password`, `sddm`, `lightdm`, etc.). The single-Enter face scan bypass for those environments continues to work exactly as it does today, unaffected by the `kde` specific token-clearing step.
3. **Graceful Degradation**: If the LinuxCamPAM module is invoked in a non-KDE context, the parser (`kde::scan_effective_stack`) and token injection (`set_empty_authtok_if_unset`) are never executed. This guarantees zero overhead and zero risk of inadvertently injecting an empty token into GDM or LightDM's authentication flow, preventing potential lockouts.
4. **Auto-Detection Safety**: The auto-detection specifically looks for `pam_kwallet5.so` in `/etc/pam.d/kde` or `/usr/lib/pam.d/kde`. On a purely GNOME system without KDE installed, this will silently result in `legacy` (no-op) and exit gracefully without raising errors.

### 6. Installation and Upgrades

The check runs on package install and upgrade and in the source installer;
its output is informational and never fails the install.

- No explicit settings, vendor KDE stack with a KWallet auth rule: `auto`
  switches the lock screen to `single_enter`. The release notes name
  `kde_lockscreen = legacy` as the opt-out.
- Manual override, custom stack, or explicit settings: behavior unchanged.
- Package removal has nothing to restore because no PAM files are modified.

The TPM feature is not part of this branch. The empty token is the only
`PAM_AUTHTOK` value LinuxCamPAM sets without user input; no secret retrieval
should be added to solve the screen-lock prompt.

## Implementation Details

The snippets follow the current code style (2-space indent, `FILE*` instead
of iostreams in anything the PAM module includes). They show intent and
interfaces; final code may differ in naming.

### 7.1 Settings: `src/pam/pam_config.hpp`

```cpp
enum class KdeLockscreenMode { Auto, SingleEnter, Legacy };

struct PamConfig {
  uid_t min_uid = linuxcampam::DEFAULT_MIN_UID;
  bool require_confirmation = true;
  std::vector<std::string> confirmation_exempt_services = {
      "gdm-password", "sddm", "lightdm", "login",
      "swaylock", "i3lock", "xscreensaver", "kscreenlocker", "kde",
      "systemd-user"};
  bool exempt_services_explicit = false;
  KdeLockscreenMode kde_lockscreen = KdeLockscreenMode::Auto;
  bool kde_lockscreen_invalid = false;
  // welcome fields unchanged
};

inline std::string_view unquote(std::string_view s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

inline std::optional<KdeLockscreenMode>
parse_kde_lockscreen(std::string_view value) {
  value = trim(unquote(trim(value)));
  if (value == "auto") return KdeLockscreenMode::Auto;
  if (value == "single_enter") return KdeLockscreenMode::SingleEnter;
  if (value == "legacy") return KdeLockscreenMode::Legacy;
  return std::nullopt;
}

inline const char *to_string(KdeLockscreenMode mode) {
  switch (mode) {
  case KdeLockscreenMode::Auto: return "auto";
  case KdeLockscreenMode::SingleEnter: return "single_enter";
  case KdeLockscreenMode::Legacy: return "legacy";
  }
  return "legacy";
}
```

In `resolve_pam_config()`:

```cpp
  if (auto ces_opt = get_value("confirmation_exempt_services")) {
    config.confirmation_exempt_services =
        split(unquote(*ces_opt), ',');
    config.exempt_services_explicit = true;
  }

  if (auto kl_opt = get_value("kde_lockscreen")) {
    if (auto mode = parse_kde_lockscreen(*kl_opt)) {
      config.kde_lockscreen = *mode;
    } else {
      config.kde_lockscreen = KdeLockscreenMode::Legacy;
      config.kde_lockscreen_invalid = true;
    }
  }
```

The INI parser does not strip inline comments, so `kde_lockscreen = auto ;
note` is an invalid value and falls back to `legacy`. Document this. An empty
explicit list (`confirmation_exempt_services =`) counts as explicit.

### 7.2 Stack Scanner and Decision: `src/pam/kde_lockscreen.hpp`

New header-only file, included by the PAM module, the CLI, and the tests.

```cpp
#pragma once
#include "pam_config.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace linuxcampam::kde {

inline constexpr std::string_view SERVICE = "kde";
inline constexpr std::string_view KWALLET_MODULE = "pam_kwallet5.so";
inline constexpr size_t MAX_STACK_BYTES = 64 * 1024;

#ifdef LINUXCAMPAM_TEST_PAM_DIR
inline constexpr const char *ETC_STACK = LINUXCAMPAM_TEST_PAM_DIR "/etc/kde";
inline constexpr const char *VENDOR_STACK =
    LINUXCAMPAM_TEST_PAM_DIR "/vendor/kde";
#else
inline constexpr const char *ETC_STACK = "/etc/pam.d/kde";
inline constexpr const char *VENDOR_STACK = "/usr/lib/pam.d/kde";
#endif

enum class Line { Other, KwalletAuth, OtherKwalletAuth, Unsupported };
enum class Scan {
  KwalletAuth, NoKwalletAuth, OtherKwalletAuth,
  Missing, Unreadable, TooLarge, Unsupported
};

struct StackInfo {
  const char *path = nullptr;
  Scan scan = Scan::Missing;
};

struct Decision {
  bool single_enter = false;
  const char *reason = "not the kde service";
};

namespace detail {
inline std::pair<std::string_view, std::string_view>
next_token(std::string_view s) {
  s = trim(s);
  auto end = s.find_first_of(" \t");
  if (end == std::string_view::npos) {
    return {s, {}};
  }
  return {s.substr(0, end), s.substr(end)};
}
} // namespace detail

inline Line classify_line(std::string_view line) {
  if (auto hash = line.find('#'); hash != std::string_view::npos) {
    line = line.substr(0, hash);
  }
  line = trim(line);
  if (line.empty() || line.front() == '@') {
    return Line::Other;
  }
  if (line.back() == '\\') {
    return Line::Unsupported;
  }

  auto [type, rest] = detail::next_token(line);
  if (!type.empty() && type.front() == '-') {
    type.remove_prefix(1);
  }
  if (type != "auth") {
    return Line::Other;
  }

  rest = trim(rest);
  if (!rest.empty() && rest.front() == '[') {
    auto close = rest.find(']');
    if (close == std::string_view::npos) {
      return Line::Unsupported;
    }
    rest = rest.substr(close + 1);
  } else {
    rest = detail::next_token(rest).second;
  }

  auto module = detail::next_token(rest).first;
  if (auto slash = module.rfind('/'); slash != std::string_view::npos) {
    module.remove_prefix(slash + 1);
  }
  if (module == KWALLET_MODULE) {
    return Line::KwalletAuth;
  }
  if (module.substr(0, 11) == "pam_kwallet") {
    return Line::OtherKwalletAuth;
  }
  return Line::Other;
}

inline Scan scan_text(std::string_view text) {
  bool kwallet = false;
  bool other = false;
  while (!text.empty()) {
    auto nl = text.find('\n');
    auto line = text.substr(0, nl);
    text = nl == std::string_view::npos ? std::string_view{}
                                        : text.substr(nl + 1);
    switch (classify_line(line)) {
    case Line::KwalletAuth: kwallet = true; break;
    case Line::OtherKwalletAuth: other = true; break;
    case Line::Unsupported: return Scan::Unsupported;
    case Line::Other: break;
    }
  }
  if (kwallet) return Scan::KwalletAuth;
  return other ? Scan::OtherKwalletAuth : Scan::NoKwalletAuth;
}

inline Scan scan_file(const char *path) {
  struct FileCloser {
    void operator()(FILE *f) const {
      if (f) {
        (void)std::fclose(f); // NOLINT
      }
    }
  };
  std::unique_ptr<FILE, FileCloser> f(std::fopen(path, "re")); // NOLINT
  if (!f) {
    return errno == ENOENT ? Scan::Missing : Scan::Unreadable;
  }
  std::string text;
  std::array<char, 4096> buf{};
  size_t n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), f.get())) > 0) {
    if (text.size() + n > MAX_STACK_BYTES) {
      return Scan::TooLarge;
    }
    text.append(buf.data(), n);
  }
  return std::ferror(f.get()) ? Scan::Unreadable : scan_text(text);
}

inline StackInfo scan_effective_stack() {
  Scan etc = scan_file(ETC_STACK);
  if (etc != Scan::Missing) {
    return {ETC_STACK, etc};
  }
  return {VENDOR_STACK, scan_file(VENDOR_STACK)};
}

inline Decision decide(const PamConfig &config, const StackInfo &stack) {
  switch (config.kde_lockscreen) {
  case KdeLockscreenMode::Legacy:
    return {false, config.kde_lockscreen_invalid
                       ? "invalid kde_lockscreen value"
                       : "kde_lockscreen = legacy"};
  case KdeLockscreenMode::SingleEnter:
    return {true, "kde_lockscreen = single_enter"};
  case KdeLockscreenMode::Auto:
    break;
  }
  switch (stack.scan) {
  case Scan::KwalletAuth: return {true, "KWallet auth rule found"};
  case Scan::NoKwalletAuth: return {false, "no KWallet auth rule"};
  case Scan::OtherKwalletAuth: return {false, "unsupported KWallet module"};
  case Scan::Missing: return {false, "kde PAM file not found"};
  case Scan::Unreadable: return {false, "kde PAM file unreadable"};
  case Scan::TooLarge: return {false, "kde PAM file too large"};
  case Scan::Unsupported: return {false, "unsupported PAM syntax"};
  }
  return {false, "unknown"};
}

inline bool is_confirmation_exempt(const PamConfig &config,
                                   std::string_view service,
                                   const Decision &kde) {
  if (kde.single_enter && service == SERVICE &&
      !config.exempt_services_explicit) {
    return false;
  }
  const auto &list = config.confirmation_exempt_services;
  return std::find(list.begin(), list.end(), service) != list.end();
}

} // namespace linuxcampam::kde
```

### 7.3 PAM Module: `src/pam/pam_linuxcampam.cpp`

Test builds compile the same source with fixed build-directory paths. The
installed module never reads paths from the environment or arguments.

```cpp
#include "kde_lockscreen.hpp"

namespace {
namespace kde = linuxcampam::kde;

#ifdef LINUXCAMPAM_TEST_SOCKET_PATH
constexpr const char *PAM_SOCKET_PATH = LINUXCAMPAM_TEST_SOCKET_PATH;
constexpr const char *PAM_CONFIG_PATH = LINUXCAMPAM_TEST_CONFIG_PATH;
#else
constexpr const char *PAM_SOCKET_PATH = linuxcampam::SOCKET_PATH;
constexpr const char *PAM_CONFIG_PATH = linuxcampam::CONFIG_PATH;
#endif

// pam_kwallet5 prompts when PAM_AUTHTOK is NULL and skips an empty token.
void set_empty_authtok_if_unset(pam_handle_t *pamh) {
  const void *tok = nullptr;
  if (pam_get_item(pamh, PAM_AUTHTOK, &tok) != PAM_SUCCESS ||
      tok != nullptr) {
    return;
  }
  if (pam_set_item(pamh, PAM_AUTHTOK, "") != PAM_SUCCESS) {
    syslog(LOG_WARNING, "Could not set empty PAM_AUTHTOK for kde");
  }
}
} // namespace
```

`load_pam_config()` and `addr.sun_path` switch to `PAM_CONFIG_PATH` and
`PAM_SOCKET_PATH`. The service lookup moves out of the confirmation block so
the decision is available on the success path:

```diff
     PamConfig config = load_pam_config(PAM_CONFIG_PATH);
+
+    const void *service_ptr = nullptr;
+    if (pam_get_item(pamh, PAM_SERVICE, &service_ptr) != PAM_SUCCESS) {
+      service_ptr = nullptr;
+    }
+    const char *service = static_cast<const char *>(service_ptr);
+
+    kde::Decision kde_mode;
+    if (service != nullptr && std::string_view{service} == kde::SERVICE) {
+      kde::StackInfo stack;
+      if (config.kde_lockscreen == KdeLockscreenMode::Auto) {
+        stack = kde::scan_effective_stack();
+      }
+      kde_mode = kde::decide(config, stack);
+      syslog(config.kde_lockscreen_invalid ? LOG_WARNING : LOG_DEBUG,
+             "kde_lockscreen: %s -> %s (%s%s%s)",
+             to_string(config.kde_lockscreen),
+             kde_mode.single_enter ? "single_enter" : "legacy",
+             kde_mode.reason, stack.path ? ", " : "",
+             stack.path ? stack.path : "");
+    }
 ...
     if (config.require_confirmation) {
-      const void *service_ptr = nullptr;
-      if (pam_get_item(pamh, PAM_SERVICE, &service_ptr) != PAM_SUCCESS || ...
+      if (service == nullptr) {
+        syslog(LOG_WARNING,
+               "PAM_SERVICE unavailable; applying confirmation prompt");
+      }
-      if (service_ptr == nullptr || std::find(...) == ...end()) {
+      if (service == nullptr ||
+          !kde::is_confirmation_exempt(config, service, kde_mode)) {
 ...
       if (resp.find("AUTH_SUCCESS") != std::string::npos) {
+        if (kde_mode.single_enter) {
+          set_empty_authtok_if_unset(pamh);
+        }
 #ifndef DISABLE_WELCOME_MESSAGE
```

The file is read only for the `kde` service and only in `auto` mode. The
greeter runs PAM as the user; both stack files are world-readable.

### 7.4 CLI: `linuxcampam check-kde`

Add a branch to `src/cli/main.cpp` and `src/pam` to the CLI include
directories. No daemon connection is needed.

```cpp
  } else if (op == "check-kde") {
    bool brief = argc > 2 && std::string_view{argv[2]} == "--brief";
    return run_check_kde(brief);
  }
```

`run_check_kde()` loads the config, scans both stack files separately, and
prints a report. Example for the test laptop after the upgrade:

```text
KDE lock screen
  Config:          /etc/linuxcampam/config.ini
  kde_lockscreen:  auto -> legacy (no KWallet auth rule, /etc/pam.d/kde)
  Confirmation:    required (explicit confirmation_exempt_services)
  Empty token:     off
  Vendor stack:    /usr/lib/pam.d/kde (KWallet auth rule found)
  Local override:  /etc/pam.d/kde (no KWallet auth rule)
Note: the local override is no longer needed for single-Enter unlock.
  Compare it with the vendor file before removing it:
  diff -u /usr/lib/pam.d/kde /etc/pam.d/kde
```

Reported conditions:

| Condition | Message and exit code |
| --- | --- |
| Effective `single_enter`, or `legacy` chosen explicitly | OK, exit 0 |
| `auto` resolved to `legacy` on a supported stack because of an override | Override note, exit 0 |
| Explicit list contains `kde` while mode is `single_enter` | Hint: remove `kde` from the list, exit 1 |
| `require_confirmation = false` | Hint: lock screen scans immediately, exit 1 |
| Other `pam_kwallet*` module, unsupported syntax, file too large | Unsupported stack, manual fallback documented, exit 1 |
| No `kde` stack but `/etc/pam.d/kscreenlocker` exists | Non-`kde` service name, not supported yet, exit 1 |
| No KDE stack at all | "KDE not installed", exit 0 |
| Config unreadable | Error, exit 2 |

`--brief` prints only the summary line and any hint, for package scripts.

### 7.5 Setup Script: `scripts/setup_config.sh`

Dispatch the new options before the immutable-flag block and camera
detection, which currently start at the top of the script:

```bash
LINUXCAMPAM_CLI=$(command -v linuxcampam || echo /usr/bin/linuxcampam)

case "${1:-}" in
    --check-kde)
        exec "$LINUXCAMPAM_CLI" check-kde "${@:2}"
        ;;
    --configure-kde)
        configure_kde "${2:-}"
        exit $?
        ;;
esac
```

`configure_kde` writes a single key in one file, so an atomic `mv` in the
same directory is enough; the original stays untouched on any failure.

```bash
configure_kde() {
    local mode=$1 dir tmp backup was_immutable=false
    case "$mode" in
        auto|single_enter|legacy) ;;
        *) echo "Usage: $0 --configure-kde auto|single_enter|legacy" >&2
           return 2 ;;
    esac
    [ "$(id -u)" -eq 0 ] || { echo "[Setup] Run as root." >&2; return 1; }
    [ -f "$CONFIG_FILE" ] || { echo "[Setup] $CONFIG_FILE missing." >&2; return 1; }

    if command -v lsattr >/dev/null &&
       lsattr "$CONFIG_FILE" 2>/dev/null | cut -c 5 | grep -q i; then
        chattr -i "$CONFIG_FILE" ||
            { echo "[Setup] Cannot clear immutable flag." >&2; return 1; }
        was_immutable=true
    fi

    dir=$(dirname "$CONFIG_FILE")
    tmp=$(mktemp "$dir/.config.ini.XXXXXX")
    trap 'rm -f "$tmp"; [ "$was_immutable" = true ] && chattr +i "$CONFIG_FILE"' RETURN INT TERM

    backup="$CONFIG_FILE.$(date +%Y%m%d_%H%M%S).bak"
    cp -p "$CONFIG_FILE" "$backup"
    awk -v mode="$mode" -f - "$CONFIG_FILE" > "$tmp" <<'AWK'
/^\[/ {
    if (in_sec && !done) { print "kde_lockscreen = " mode; done = 1 }
    in_sec = ($0 ~ /^\[Security\][[:space:]]*$/)
}
in_sec && /^[[:space:]]*kde_lockscreen[[:space:]]*=/ {
    if (!done) { print "kde_lockscreen = " mode; done = 1 }
    next
}
{ print }
END {
    if (!done) {
        if (!in_sec) print "\n[Security]"
        print "kde_lockscreen = " mode
    }
}
AWK
    chmod --reference="$CONFIG_FILE" "$tmp"
    chown --reference="$CONFIG_FILE" "$tmp"
    mv -f "$tmp" "$CONFIG_FILE"
    echo "[Setup] kde_lockscreen = $mode (backup: $backup)"
    "$LINUXCAMPAM_CLI" check-kde --brief || true
}
```

The key is written to `[Security]`, which wins over other sections in
`get_value()`. A commented `; kde_lockscreen = ...` line is left in place.

### 7.6 Packaging

`debian/postinst`, after `pam-auth-update` and outside the fresh-install
block so it also runs on upgrades:

```sh
    if [ -x /usr/bin/linuxcampam ]; then
        /usr/bin/linuxcampam check-kde --brief || true
    fi
```

`scripts/install.sh` runs the same command after installing the CLI.
`debian/prerm` needs no change. `CMakeLists.txt`:

```cmake
target_include_directories(linuxcampam PRIVATE include src/pam)
```

The existing `install(PROGRAMS ... setup_config.sh ...)` already ships the
setup helper; the CLI is already installed. No new installed files.

### 7.7 Default Configuration: `config/config.ini`

In `[Security]`, after the existing confirmation options, and with the
documented default list corrected:

```ini
; Comma-separated list of PAM services that skip the confirmation prompt.
; Setting this key overrides kde_lockscreen's handling of "kde".
; Default is "gdm-password,sddm,lightdm,login,swaylock,i3lock,xscreensaver,kscreenlocker,kde,systemd-user"
; confirmation_exempt_services = "gdm-password,sddm,lightdm,login,swaylock,i3lock,xscreensaver,kscreenlocker,kde,systemd-user"

; KDE lock screen behavior (PAM service "kde").
;   auto         - single_enter if the kde PAM stack has a pam_kwallet5
;                  auth rule, legacy otherwise
;   single_enter - press Enter to scan, unlock without a KWallet prompt
;   legacy       - scan as soon as the lock screen appears (0.9.7.5)
; Do not put a comment on the same line as the value.
; Default is auto.
; kde_lockscreen = auto
```

## Tests

### 8.1 Unit Tests

Extend `tests/test_pam_logic.cpp` and add `tests/test_kde_lockscreen.cpp` to
`linuxcampam_tests` in `CMakeLists.txt`.

Configuration:

- `KdeLockscreenDefaultsToAuto`, `KdeLockscreenParsesAllValues`,
  `KdeLockscreenQuotedValue`, `KdeLockscreenInvalidFallsBackToLegacy`,
  `KdeLockscreenInlineCommentIsInvalid`.
- `ExemptListDefaultIsNotExplicit`, `ExemptListSetIsExplicit`,
  `EmptyExemptListIsExplicit`, `SecuritySectionWinsForKdeLockscreen`.
- `DefaultExemptListContents`: the full list, element by element.

Scanner and decision, using inline strings:

```cpp
TEST(KdeStack, VendorKubuntuStackHasKwalletAuth) {
  constexpr std::string_view stack = R"(
auth    requisite       pam_nologin.so
auth    required        pam_succeed_if.so user != root quiet_success
@include common-auth
auth    optional        pam_kwallet5.so
@include common-account
session optional        pam_kwallet5.so auto_start
)";
  EXPECT_EQ(kde::scan_text(stack), kde::Scan::KwalletAuth);
}

TEST(KdeStack, SessionRuleAloneDoesNotCount) {
  EXPECT_EQ(kde::scan_text("session optional pam_kwallet5.so auto_start\n"),
            kde::Scan::NoKwalletAuth);
}

TEST(KdeDecision, ExplicitListKeepsKdeExempt) {
  PamConfig config;
  config.exempt_services_explicit = true;
  kde::Decision single{true, "test"};
  EXPECT_TRUE(kde::is_confirmation_exempt(config, "kde", single));
}
```

Further cases: commented rule, `-auth` prefix, bracketed control with
spaces, absolute module path, `pam_kwallet.so`/`pam_kwallet6.so` reported as
`OtherKwalletAuth`, trailing backslash, unterminated bracket, CRLF line
endings, and the manual override (no auth rule). `scan_file` cases use files
in a temporary directory: missing, unreadable (`chmod 000`, skipped as root),
exactly 64 KiB, 64 KiB + 1. `decide` is tested for every mode and scan
result; `is_confirmation_exempt` for `kde` and a non-`kde` service in both
decisions, with and without an explicit list.

### 8.2 PAM Integration Harness

Exercises the real module through Linux-PAM with `pam_start_confdir()`
(Linux-PAM 1.4 or newer; 24.04 ships 1.5.3, 26.04 ships 1.7.0). Nothing is
installed and no system PAM file is read.

Layout under `${CMAKE_BINARY_DIR}/pam_it/`:

```text
pam_it/
  config.ini            written per test
  socket                fake daemon
  etc/kde               service file, also read by auto detection
  modules/
    pam_linuxcampam_it.so
    pam_kwallet5.so     fake, named like the real module on purpose
    pam_fake_unix.so
```

Service file written by the tests, matching the Debian jump layout:

```text
auth [success=2 default=ignore] @MODULES@/pam_linuxcampam_it.so no_welcome
auth [success=1 default=ignore] @MODULES@/pam_fake_unix.so
auth requisite pam_deny.so
auth required  pam_permit.so
auth optional  @MODULES@/pam_kwallet5.so
```

Fake KWallet module, reproducing the two branches that matter:

```cpp
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
```

`pam_fake_unix.so` calls `pam_get_authtok()` and accepts only `secret`. The
fake daemon is a thread listening on `pam_it/socket` that counts requests and
answers `AUTH_SUCCESS` or a failure string chosen by the test. The test
conversation function replays scripted answers and records every prompt.
The test reads `pam_getenv(pamh, "FAKE_KWALLET")` before `pam_end()`.

| Test | Setup | Expected |
| --- | --- | --- |
| `AutoSingleEnterFaceSuccess` | `auto`, KWallet rule, reply `""`, daemon success | `PAM_SUCCESS`; 1 prompt; `FAKE_KWALLET=empty` |
| `LegacyReproducesSecondPrompt` | `legacy`, same stack | `PAM_SUCCESS`; only the "Password: " prompt; `prompted` |
| `AutoWithoutKwalletRuleIsLegacy` | `auto`, rule removed | no confirmation prompt; env unset |
| `TypedPassword` | reply `secret` | `PAM_SUCCESS`; 0 daemon requests; `password` |
| `WrongPassword` | reply `wrong` | `PAM_AUTH_ERR` |
| `FaceFailFallsBackToPassword` | daemon failure, then `secret` | `PAM_SUCCESS`; `password` |
| `DaemonDown` | no socket, then `secret` | `PAM_SUCCESS`; `password` |
| `CancelledConfirmation` | conversation returns `PAM_CONV_ERR` | not success; 0 daemon requests |
| `TokenClearedBetweenAttempts` | same handle: `wrong`, then `""` + success | second attempt `FAKE_KWALLET=empty` |
| `NonKdeServiceUntouched` | service `sudo`, `single_enter` | confirmation as before; `prompted` |
| `ExplicitListWithKde` | `single_enter`, list includes `kde` | no confirmation; `empty` |

CMake sketch:

```cmake
if(TARGET gtest)
  set(PAM_IT_DIR ${CMAKE_BINARY_DIR}/pam_it)

  add_library(pam_linuxcampam_it MODULE src/pam/pam_linuxcampam.cpp)
  target_include_directories(pam_linuxcampam_it PRIVATE
      include src/common ${PAM_INCLUDE_DIR})
  target_link_libraries(pam_linuxcampam_it PRIVATE ${PAM_LIBRARY})
  target_compile_definitions(pam_linuxcampam_it PRIVATE
      LINUXCAMPAM_TEST_PAM_DIR="${PAM_IT_DIR}"
      LINUXCAMPAM_TEST_SOCKET_PATH="${PAM_IT_DIR}/socket"
      LINUXCAMPAM_TEST_CONFIG_PATH="${PAM_IT_DIR}/config.ini")

  add_library(pam_fake_kwallet MODULE tests/pam_it/pam_fake_kwallet.cpp)
  add_library(pam_fake_unix MODULE tests/pam_it/pam_fake_unix.cpp)
  set_target_properties(pam_linuxcampam_it pam_fake_kwallet pam_fake_unix
      PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ${PAM_IT_DIR}/modules)
  set_target_properties(pam_fake_kwallet PROPERTIES OUTPUT_NAME pam_kwallet5)
  target_link_libraries(pam_fake_kwallet PRIVATE ${PAM_LIBRARY})
  target_link_libraries(pam_fake_unix PRIVATE ${PAM_LIBRARY})

  add_executable(pam_integration_tests tests/pam_it/test_kde_flow.cpp)
  target_link_libraries(pam_integration_tests PRIVATE
      gtest gtest_main ${PAM_LIBRARY} pthread)
  target_compile_definitions(pam_integration_tests PRIVATE
      PAM_IT_DIR="${PAM_IT_DIR}")
  add_dependencies(pam_integration_tests
      pam_linuxcampam_it pam_fake_kwallet pam_fake_unix)
  add_test(NAME PamIntegration COMMAND pam_integration_tests)
endif()
```

None of the `_it` targets has an `install()` rule. A CI step checks that the
installed module carries no test path:
`! strings pam_linuxcampam.so | grep -q pam_it`.

### 8.3 Script Tests

`tests/scripts/test_configure_kde.sh`, run with a temporary `CONFIG_FILE`
(the variable becomes overridable only when `LINUXCAMPAM_TEST=1`):

- key absent, commented, present in `[Security]`, present in another
  section, no `[Security]` section;
- invalid mode rejected without changes;
- `mv` failure simulated through a read-only directory: original unchanged,
  temporary file removed;
- immutable flag cleared and restored (privileged, disposable VM only).

Plus `bash -n` and `shellcheck` on both scripts.

## Documentation

- `docs/CONFIGURATION.md`: replace the stale list (`kdm` removed) with the
  real default list; add a "KDE lock screen" subsection with the mode table,
  precedence rules, upgrade note, and the manual PAM fallback.
- `docs/man/linuxcampam.conf.5.md`: add `require_confirmation`,
  `confirmation_exempt_services`, and `kde_lockscreen`.
- `docs/man/pam_linuxcampam.8.md`: describe the confirmation prompt and the
  empty token, and list the files read for `kde`.
- `docs/man/linuxcampam.1.md`: add `check-kde [--brief]`.
- `docs/IR_CAMERA_TROUBLESHOOTING.md`: check the physical shutter and black
  frames first.
- `README.md`: one line under KDE support pointing to the configuration
  guide.
- `docs/adr/0005-empty-authtok-for-kde-lock-screen.md`: context (the two
  prompts), options considered (PAM file override, `PAM_KWALLET5_LOGIN`
  environment shortcut, empty token), decision, and consequences, in the
  format of the existing ADRs.

Draft `CHANGELOG.md` entry:

```markdown
## [Unreleased]

### Added

- **KDE lock screen**: New `kde_lockscreen` setting (`auto`, `single_enter`,
  `legacy`). In `single_enter`, press Enter to scan; a recognized face
  unlocks without a second Enter. `auto` enables this when the KDE PAM stack
  contains a `pam_kwallet5.so` auth rule.
- **`linuxcampam check-kde`**: Read-only report of the effective KDE
  lock-screen behavior; also available as
  `linuxcampam-setup-config --check-kde`.

### Changed

- On upgrade, systems with the stock Kubuntu KDE PAM stack and no explicit
  `confirmation_exempt_services` switch to `single_enter`. Set
  `kde_lockscreen = legacy` to keep the previous behavior.

### Docs

- Corrected the documented default `confirmation_exempt_services` list.
```

## Expected File Scope

| Area | Files | Change |
| --- | --- | --- |
| Settings | `src/pam/pam_config.hpp` | `KdeLockscreenMode`, `exempt_services_explicit`, `unquote`, parsing. |
| Detection | `src/pam/kde_lockscreen.hpp` (new) | Scanner, decision, exemption helper. |
| PAM module | `src/pam/pam_linuxcampam.cpp` | Early service lookup, decision, empty token, test path macros. |
| CLI | `src/cli/main.cpp` | `check-kde [--brief]`. |
| Setup | `scripts/setup_config.sh` | `--check-kde`, `--configure-kde MODE`. |
| Packaging | `debian/postinst`, `scripts/install.sh`, `CMakeLists.txt` | Run the check; CLI include path; test targets. |
| Config | `config/config.ini` | New key, corrected list comment. |
| Tests | `tests/test_pam_logic.cpp`, `tests/test_kde_lockscreen.cpp`, `tests/pam_it/*`, `tests/scripts/test_configure_kde.sh` | Unit, integration, script tests. |
| Docs | see the Documentation section | |

Keep unrelated code cleanup and the existing TPM work out of this PR.

## Manual Verification

On the Kubuntu 26.04 test laptop, after installing the built package:

```bash
# 1. Manual override still in place: behavior must not change.
linuxcampam check-kde
loginctl lock-session          # Enter, scan, immediate unlock
journalctl -b --since -5min --grep 'LinuxCamPAM|pam_kwallet5'

# 2. Vendor stack: auto must select single_enter.
sudo mv /etc/pam.d/kde /root/kde.pam.manual-override
linuxcampam check-kde          # expect: auto -> single_enter
loginctl lock-session
journalctl -b --since -5min --grep 'pam_kwallet5'
# expect "Empty or missing password, doing nothing", no "Couldn't get password"

# 3. Built-in list path: drop the explicit exemption list.
sudo linuxcampam-setup-config --configure-kde auto
# remove confirmation_exempt_services by hand (chattr -i/+i), lock again

# 4. Opt-out.
sudo linuxcampam-setup-config --configure-kde legacy
loginctl lock-session          # scan starts at once, as in 0.9.7.5

# 5. Restore the previous state.
sudo mv /root/kde.pam.manual-override /etc/pam.d/kde
```

For each step also check: typed password unlocks, wrong password is
refused, closed shutter falls back to password, resume from suspend, SDDM
login, `sudo` (confirmation prompt unchanged), and that KWallet is open in
the session after an SDDM password login.

Validate Kubuntu 24.04/Plasma 5 before releasing. Its vendor stack is also
expected to resolve to `single_enter`; if the confirmation flow misbehaves
there, `auto` needs an additional guard before release.

Plasma 6.6 introduced plasma-login-manager, which uses the `plasmalogin` PAM
service. The test laptop uses SDDM, so this is out of scope; record
`plasmalogin` exemption support as a follow-up.

Run the C++ tests, the integration harness, the script tests, Markdown lint,
and `git diff --check`. Verify the Debian package and source installation in
a disposable VM. A unit test or successful build alone does not establish
desktop unlock behavior.

## Suggested Implementation Sequence

Each step is a separate commit that builds and passes tests on its own.

1. `pam_config.hpp` parsing and `kde_lockscreen.hpp` with unit tests. No
   behavior change yet.
2. PAM module: early service lookup and exemption helper, still in effect
   `legacy` (decision forced off). Integration harness with the legacy tests.
3. Enable the decision and the empty token; add the remaining integration
   tests and the ADR.
4. `check-kde` CLI, setup script options, script tests.
5. Packaging hooks, `config.ini`, documentation, changelog.
6. Package build, VM install/upgrade test, manual verification on the laptop.

## Review Decisions

- Confirm `kde_lockscreen` with values `auto` (default), `single_enter`, and
  `legacy`; `legacy` matches 0.9.7.5 exactly and unknown values fall back
  to it.
- Confirm that `auto` selects `single_enter` only for an active `auth` rule
  with exactly `pam_kwallet5.so` in the `kde` file itself, read at
  authentication time; includes are not followed.
- Confirm that explicit `confirmation_exempt_services` and
  `require_confirmation` keep precedence over the mode.
- Confirm the empty `PAM_AUTHTOK` step: `kde` only, after a successful scan
  only, and only when the token is unset.
- Confirm that LinuxCamPAM no longer edits PAM files; the override is a
  documented manual fallback and existing overrides stay untouched.
- Confirm that the check lives in the C++ CLI and the setup script only
  delegates to it.
- Decide whether to include `--configure-kde MODE` or rely on manual
  `config.ini` edits.
- Confirm the test-only module build with compile-time paths and the CI
  check that the installed module contains none of them.
- Confirm that `plasmalogin` support, if needed, is a separate follow-up.
- Confirm that screen unlocking and login-time wallet unlocking remain
  separate responsibilities, including when the TPM branch is later merged.

After review, implementation should follow this scope and record any necessary
changes to these decisions. No implementation, commit, push, or additional
live-system modification is part of preparing this plan.
