# Threat Modelling & Risk Assessment

* **Quick Links:** [README](../README.md) | [Security Policy](../SECURITY.md) | [Configuration Guide](CONFIGURATION.md)

## 1. Executive Summary

This document details the security posture, threat model, and risk assessment for **LinuxCamPAM**. It breaks down what the system protects against, where its vulnerabilities lie, and the trade-offs made in its design.

Biometrics inherently balance convenience and security. LinuxCamPAM is designed to secure personal Linux workstations against local, opportunistic threats. It is not intended to secure highly classified environments against nation-state actors.

## 2. System Description & Trust Boundaries

### 2.1 Assets Protected

* **System Access:** Privileges granted via `sudo`, login sessions, and display managers (GDM, SDDM).
* **Biometric Data:** Face embeddings stored locally.
* **Privacy:** Preventing unauthorized activation of the camera feed.

### 2.2 Trust Boundaries & Process Isolation

* **Thin PAM Client vs. Daemon Isolation:** The PAM module (`pam_linuxcampam.so`) runs in the process space of the calling application (e.g., `sudo`, `gdm`). It behaves strictly as a thin IPC client that transmits the username and receives a boolean success/fail response. This isolates the heavy OpenCV and ONNX runtime dependencies (which have a large codebase and complex parsing attack surface) to the root `linuxcampamd` daemon.
* **User Space vs. Root Space:** The IPC socket bridges the user space calling process and the root daemon (`linuxcampamd`).
* **Software vs. Hardware:** The daemon trusts the `V4L2` kernel subsystem for video frame delivery.

## 3. Threat Modeling (STRIDE Methodology)

We use the STRIDE framework to analyze potential threats to the authentication pipeline.

### 3.1 Spoofing (Presentation Attacks)

* **Threat:** An attacker presents a photo, video, or 3D mask to the camera to impersonate an enrolled user.
* **Mitigation:**
  * **Dual Camera (IR + RGB):** High resistance. Validates overall frame brightness to ensure active IR illumination. Screens and standard photos appear black/dark in near-IR, effectively acting as a liveness check. Note that this relies on 2D IR reflection variance rather than enterprise-grade 3D depth mapping (structured light).
  * **Single IR:** Medium-High resistance. Vulnerable to specific IR-reflective printed masks.
  * **Single RGB:** Low resistance (VULNERABLE). Easily spoofed by a high-resolution video on a smartphone. *Not recommended for `sudo`.*

### 3.2 Tampering

* **Threat 1 (Data Modification):** An attacker with local access modifies the face embeddings (`/etc/linuxcampam/users/*.json`) or the ONNX AI models to inject their own face or backdoor the recognition.
* **Mitigation:** The `/etc/linuxcampam/users/` directory is secured with `0700` permissions (root-only).
* **Residual Risk:** If an attacker already has `root` access, they can modify these files. The system lacks hardware-backed cryptographic binding (like a TPM) which would tightly couple biometric data to specific hardware. (Future roadmap: HMAC integrity checks).

* **Threat 2 (Evil Maid / Camera Swap):** An attacker with physical access replaces the trusted IR camera with a malicious USB device that streams pre-recorded face videos.
* **Mitigation:** Rely on physical security. Hardware trust issue. Device ID verification (VID/PID/Serial) provides partial mitigation (see [Roadmap #5](#5-future-security-enhancements-roadmap)).

* **Threat 3 (USB Bus Frame Injection):** An attacker with physical access uses a hardware tap to intercept or inject raw video frames over the USB bus.
* **Mitigation:** The software inherently trusts the `V4L2` video stream. Unlike some enterprise biometric setups, generic webcams do not provide encrypted sensor links to the OS.
  * **Active Liveness Check**: Active liveness detection (challenge-response) provides a strong defense against frame injection by requiring unpredictable real-time user reactions, preventing the replay of static or looped video frames (see [Roadmap #6](#5-future-security-enhancements-roadmap)).
  * **System-Level Recommendation**: For highly sensitive environments, administrators are strongly advised to deploy `usbguard` to block unauthorized USB devices from enumerating, preventing hot-plug camera swap attacks.

* **Threat 4 (Configuration Tampering):** An attacker with local access modifies `/etc/linuxcampam/config.ini` to bypass security policies. For example, they could lower the similarity `threshold` to `0.01` to accept any image, switch the policy to `lenient` to bypass IR liveness checks, or point `camera_path_ir` to a virtual webcam device looping a pre-recorded spoof video.
* **Mitigation:** The configuration file must be strictly owned by `root:root` with `0644` or `0600` permissions.
  * **Advanced Mitigation (Immutable Flag):** Administrators are highly encouraged to apply the immutable flag (`chattr +i /etc/linuxcampam/config.ini`) to prevent even a compromised root daemon or rogue script from modifying these critical security parameters (see [Roadmap #9](#5-future-security-enhancements-roadmap)). The setup script provides an interactive prompt to enable this easily.

### 3.3 Repudiation

* **Threat:** A user claims they did not authenticate a `sudo` action.
* **Mitigation:** The daemon logs authentication successes and failures (via syslog/journald).

### 3.4 Information Disclosure

* **Threat 1 (Biometric Theft & Replay):** Stealing face embeddings to clone an identity or replay authentication on another system.
* **Mitigation:** Embeddings are 128-dimensional mathematical abstractions, not raw photos. Stored in root-owned directories (`0700` parent, `0600` files). Raw images are *never* saved unless explicitly enabled for debugging by an administrator.
* **Residual Risk:** While filesystem permissions protect the data at rest, biometric data is inherently exposed. While RGB faces are widely public (e.g., social media), IR faces are not. However, an attacker can still perform a hidden capture using an IR-capable camera near the victim. Because the underlying ONNX models and OpenCV tools are open-source, the attacker can independently recreate the exact same embedding matrix offline. Unlike passwords, biometric identifiers cannot be revoked or changed if compromised; they are physical identifiers, not cryptographic secrets.
* **Threat 2 (Socket Snooping):** The IPC socket (`/run/linuxcampam/socket`) has `0666` permissions.
* **Mitigation:** The socket only transmits usernames and boolean results (`AUTH_SUCCESS`/`AUTH_FAIL`). No biometric data or video frames traverse the socket.

### 3.5 Denial of Service (DoS)

* **Threat:** An attacker spams the socket with authentication requests to exhaust system resources or lock the camera.
* **Mitigation:**
  * **Timeouts:** Hard timeouts (default 3s) on authentication requests.
  * **Per-User Lockout:** Configurable lockout after *N* failed attempts (e.g., 5 failures triggers a 5-minute lockout). State is tracked in-memory.

### 3.6 Elevation of Privilege

* **Threat 1 (Daemon Exploitation):** Exploiting a vulnerability (e.g., buffer overflow) in the root-level `linuxcampamd` daemon via the user-accessible IPC socket.
* **Mitigation:**
  * Strict input sanitization on the socket (usernames must match `^[a-zA-Z0-9_\.-]+$`) preventing Path Traversal.
  * Use of modern C++ RAII patterns to prevent memory leaks and buffer overflows.
  * Compiler hardening (PIE, Stack Canaries).

* **Threat 2 (Unauthorized IPC Command Execution):** The IPC socket (`/run/linuxcampam/socket`) is world-writable (`0666`) to allow unprivileged local users to request authentication. An attacker can connect directly to the socket and issue administrative commands (e.g., `REMOVE_EMBEDDING`, `TRAIN_USER`, `SET_LOG_LEVEL`) to tamper with or disable authentication for other users.
* **Mitigation:** The daemon enforces `SO_PEERCRED` verification at the Unix socket layer, ensuring that administrative commands are only executed if the caller is `root` or matches the target user's UID. The CLI client also verifies privileges locally.
* **Residual Risk:** Low. Protected by kernel-level socket credentials.

* **Threat 3 (Virtual Hardware Injection):** The daemon uses `/dev/uinput` to emit virtual keystrokes (`KEY_WAKEUP`) for proximity waking. If the daemon is compromised, an attacker could attempt to leverage this file descriptor to inject arbitrary keystrokes (e.g., typing commands) into the host OS.
* **Mitigation:** The daemon strictly drops capabilities during `VirtualKeyboard` initialization, registering only the `KEY_WAKEUP` bit. The kernel drops any attempts to inject unregistered keys, containing the blast radius.

* **Threat 4 (Command Execution Injection):** The proximity lock feature executes the user-defined `lock_command`. If an unprivileged attacker can modify `/etc/linuxcampam/config.ini`, they could attempt to inject shell commands.
* **Mitigation:** The daemon bypasses the system shell entirely by securely tokenizing the command and executing it via `posix_spawnp()`. Shell metacharacters (`|`, `;`, `&&`) are treated as literal strings, completely eliminating shell command injection. Furthermore, the configuration file must be strictly owned by `root:root` with `0644` or `0600` permissions.
  * **Advanced Mitigation (Immutable Flag)**: Setting the immutable flag (`chattr +i /etc/linuxcampam/config.ini`) is highly recommended to block even compromised `root` processes from modifying the file (see [Roadmap #9](#5-future-security-enhancements-roadmap)). The setup script provides an interactive prompt to enable this easily.
  * **Advanced Mitigation (MAC / Read-Only)**: AppArmor/SELinux profiles and read-only mounts can further restrict modification to authorized utilities and administrators.

### 3.7 Authorization Bypass (Physical Proximity)

* **Threat:** A user leaves their machine unattended. Under standard Desktop Environment conditions, the machine remains unlocked until the screensaver timeout triggers (often 5 to 15 minutes), leaving a massive vulnerability window for an opportunistic attacker with physical access.
* **Mitigation:** LinuxCamPAM's proximity lock feature *drastically reduces* this vulnerability window. By aggressively polling for physical absence, it locks the machine within a configurable `lock_timeout_seconds` (default: 10s). While an attacker could theoretically access the machine during this short 10-second window, it represents a massive security improvement over relying on standard OS idle timers. Disabling the proximity lock feature simply reverts the system to the typical native OS behavior.

### 3.8 Automated Wake & Unlock Vulnerabilities (Zero-Interaction)

* **Threat 1 (Physical Coercion / "Rubber Hose" Attack):** An attacker physically forces the authorized user into the camera's field of view. Because the facial authentication PAM module runs passively, the machine unlocks without conscious cooperation or active intent from the victim (even if automatic waking is disabled, the attacker simply needs to wiggle the mouse to wake the screen before forcing the victim's face into view).
* **Mitigation:**
  * **Fallback / Kill Switch:** Future roadmap feature to disable PAM facial recognition temporarily via hotkey.
  * **Current Recommendation:** This is an inherent risk of all biometric authentication (including fingerprints and iris scans). For environments with high physical coercion risks, users should disable LinuxCamPAM for login/sudo entirely and rely on passwords or hardware tokens with PINs.

* **Threat 2 (Unintended Unlocks / "Walk-By"):** The user explicitly locks the workstation (e.g., `Super + L`) because an untrusted individual is nearby, but the user remains in the room within the camera's FOV. The proximity sensor instantly wakes the screen and unlocks it again, inadvertently granting the bystander access.
* **Mitigation:**
  * **Intent to Unlock:** Disable `always_wake_on_presence_detected` so presence merely wakes the screen, but authentication waits for a keystroke.
  * **Lock Cooldowns (Roadmap):** Implementing a strict lock cooldown period where facial authentication is paused immediately following an explicit manual lock event.

* **Threat 3 (Amplified Spoofing Window):** Automated wake and unlock constantly polls when presence is detected. An attacker can repeatedly present high-resolution masks or photos at their leisure while the user is away, increasing the window for brute-forcing the neural network.
* **Mitigation:** This is actively mitigated by the daemon's existing brute-force protection (per-user lockout mechanism). After a configurable number of failed recognition attempts, the system enforces a strict cooldown period, neutralizing rapid spoofing attempts. Furthermore, IR liveness checks severely degrade the success rate of presentation attacks.

### 3.9 Silent Privilege Escalation (Confused Deputy)

* **Threat:** A malicious background script attempts to silently escalate privileges via `pkexec` or `sudo`. If the authorized user happens to be sitting in front of the computer, the camera passively authenticates the request without the user's knowledge or active intent, inadvertently granting `root` access to the malware.
* **Mitigation:**
  * **Universal Confirmation:** The `require_confirmation` configuration (enabled by default) forces the PAM module to display an interactive prompt requiring the user to physically press `<Enter>` to confirm their intent *before* engaging the camera. This actively neutralizes silent escalation attacks for all non-exempt PAM services.
  * **Seamless Password Bypass:** The confirmation prompt now captures keystrokes (`"Press <Enter> for face auth, or type password:"`). If a user inputs a password, the module immediately aborts face authentication and passes the password securely (`PAM_AUTHTOK`) to the next module in the PAM stack (e.g., `pam_unix.so`). This avoids double-prompting while providing an instant opt-out for environments where users temporarily prefer passwords.
  * **Service Exemptions:** To prevent user fatigue, explicitly intentional authentication services (e.g., login managers like `gdm-password`, screen lockers like `swaylock`) are exempted from this confirmation prompt via the `confirmation_exempt_services` list.
* **Residual Risk:** Fully mitigated by default. Administrators who manually add `sudo` or `polkit-1` to the exemption list actively assume this risk.

## 4. Risk Assessment Matrix

### 4.1 Methodology

Overall Risk is calculated by combining **Likelihood** (Low, Medium, High) and **Impact** (Low, Medium, Critical) using a standard qualitative matrix:

* **Critical Risk:** High Likelihood + Critical Impact.
* **High Risk:** High Likelihood + High Impact, or Medium Likelihood + Critical Impact.
* **Medium Risk:** Low Likelihood + Critical/High Impact, or Medium Likelihood + Medium Impact.
* **Low Risk:** Low/Medium Likelihood + Low Impact.

### 4.2 Risk Matrix

| Threat Category | Specific Threat | Likelihood | Impact | Overall Risk | Current Mitigation Status | Mitigation Notes |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Spoofing** | RGB Photo/Video Replay | High (if RGB used) | High | **Critical** | Partially | Warn users against RGB-only. Active Liveness Detection (see [Roadmap #6](#5-future-security-enhancements-roadmap)) mitigates this via randomized physical challenges, defeating static photos and pre-recorded videos. |
| **Spoofing** | Advanced 3D/Heated Mask | Low | High | **Medium** | Partially | IR Liveness detection provides baseline defense. Active Liveness Detection (see [Roadmap #6](#5-future-security-enhancements-roadmap)) mitigates this by requiring dynamic facial movements. |
| **Tampering** | Root User Modifies Embeddings | Low | High | **Medium** | WiP | Relies on OS boundaries; attacker already has root. (HMAC/encryption planned, see [Roadmap #1](#5-future-security-enhancements-roadmap), [#8](#5-future-security-enhancements-roadmap)) |
| **Tampering** | Evil Maid (USB Camera Swap) | Low | High | **Medium** | Partially | Hardware trust issue. Mitigate via BIOS passwords. Device ID verification (VID/PID/Serial) provides partial mitigation (see [Roadmap #5](#5-future-security-enhancements-roadmap)). |
| **Tampering** | USB Bus Frame Injection | Low | High | **Medium** | No | No encrypted sensor links. Device ID checks do not mitigate bus taps. Challenge-response mitigates pre-recorded frame loops (see [Roadmap #6](#5-future-security-enhancements-roadmap)). Recommend `usbguard` for physical port control. |
| **Tampering** | Configuration Tampering (Bypass) | Low | Critical | **Medium** | Yes | Defended by root OS permissions. Immutable flag (`chattr +i`) highly recommended (prompt available via setup script). |
| **Repudiation** | Sudo Authentication Denial | Low | Low | **Low** | Yes | Daemon logs authentication successes and failures via syslog. |
| **Info Disclosure** | Read Socket Traffic | Medium | Low | **Low** | Yes | Socket only sends booleans and usernames. |
| **Info Disclosure** | Biometric Embedding Theft | Low | High | **Medium** | Partially | Secured by root permissions. Encryption planned (see [Roadmap #8](#5-future-security-enhancements-roadmap)). Note: Attackers can still recreate embeddings offline from public RGB photos or hidden IR captures. |
| **DoS** | Auth Request Spamming | Low | Medium | **Low** | Yes | Per-user lockout prevents resource exhaustion. An attacker triggering the lockout forces a fallback to password (safe failure), preserving system security. |
| **Elevation** | Buffer Overflow in Daemon | Low | Critical | **Medium** | Yes | Modern C++, sanitizers, strict socket parsing. |
| **Elevation** | Unauthorized IPC Commands | Medium | High | **Low** | Yes | Implemented socket peer credential verification (`SO_PEERCRED`) in daemon to prevent unauthorized administration. |
| **Elevation** | Virtual Hardware Injection (`uinput`) | Low | High | **Medium** | Yes | Kernel-level capability dropping restricts injection to `KEY_WAKEUP`. |
| **Elevation** | Config Command Injection | Low | Low | **Low** | Yes | Defeated architecturally by `posix_spawnp()` tokenization, bypassing `/bin/sh`. |
| **Bypass** | Physical Access during OS Idle Timeout | High | High | **Critical** | Yes | LinuxCamPAM proximity lock reduces the typical native 5-15 minute vulnerability window down to seconds (`lock_timeout_seconds`). Disabling it reverts to typical OS behavior. |
| **Zero-Interaction** | Physical Coercion (Forced Unlock) | Low | Critical | **Medium** | Accepted Risk | Inherent to all biometrics (including fingerprints). Falls under organizational Risk Appetite; if unacceptable, disable LinuxCamPAM entirely. |
| **Zero-Interaction** | Unintended Unlock (Walk-By after Lock) | Medium | High | **High** | No | User explicitly locks but remains in FOV. Disable zero-interaction unlock if at risk. |
| **Zero-Interaction** | Amplified Spoofing Window | Medium | High | **Medium** | Yes | Polling increases attack window, but is effectively neutralized by existing auth lockouts and IR liveness checks. |
| **Zero-Interaction** | Silent Privilege Escalation | Medium | Critical | **High** | Yes | `require_confirmation` blocks all unexpected services. Does not apply to exempt login managers. |

## 5. Future Security Enhancements (Roadmap)

To further harden the LinuxCamPAM architecture, the following enhancements are planned:

1. **Embedding Integrity (HMAC):** Implement cryptographic signatures on user embedding files to detect unauthorized tampering.
2. **Model Verification:** Enforce SHA256 hash verification of ONNX model files upon load to prevent the injection of backdoored models.
3. **SELinux / AppArmor Profiles:** Provide strict Mandatory Access Control (MAC) policies confining the daemon strictly to required devices (`/dev/video*`) and sockets, neutralizing potential privilege escalation.
4. **Configurable Logging Restrictions:** Allow administrators to disable the logging of usernames in production to prevent identity leakage via syslog.
5. **Hardware ID Verification:** Validate USB Vendor ID (VID), Product ID (PID), and unique Serial Number (where supported by UVC descriptors) to detect and block unauthorized camera hardware swaps.
6. **Active Liveness Detection (Challenge-Response):** Implement randomized user prompts (e.g., "blink", "turn head") to thwart video replay injection attacks by forcing unpredictable live interaction.
7. **Embedding Encryption & Hardware Binding:** Encrypt user embedding files locally using a host key or hardware-backed key (via TPM) to protect them from offline extraction and relocation.
8. **Privilege Separation:** Transition the daemon from running as `root` to a dedicated `linuxcampam` service user, with hardware access restricted via standard Linux groups (`video`, `render`).
9. **D-Bus Screen-State Synchronization:** Integrate with the desktop environment's session bus (e.g., `org.freedesktop.login1`) to track explicit manual lock events, enabling strict lock cooldown periods to prevent unintended walk-by unlocks.
10. **Default Immutable Configuration:** Future major releases will lock `/etc/linuxcampam/config.ini` by default (`chattr +i`) upon installation completion to aggressively block unauthorized modifications. *(Note: The installation script already supports an interactive opt-in prompt for this feature).*
