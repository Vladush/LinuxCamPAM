# Threat Modelling & Risk Assessment


## 1. Executive Summary

This document details the security posture, threat model, and risk assessment for **LinuxCamPAM**. It breaks down what the system protects against, where its vulnerabilities lie, and the trade-offs made in its design. 

Biometrics inherently balance convenience and security. LinuxCamPAM is designed to secure personal Linux workstations against local, opportunistic threats. It is not intended to secure highly classified environments against nation-state actors.

## 2. System Description & Trust Boundaries

### 2.1 Assets Protected
*   **System Access:** Privileges granted via `sudo`, login sessions, and display managers (GDM, SDDM).
*   **Biometric Data:** Face embeddings stored locally.
*   **Privacy:** Preventing unauthorized activation of the camera feed.

### 2.2 Trust Boundaries & Process Isolation
*   **Thin PAM Client vs. Daemon Isolation:** The PAM module (`pam_linuxcampam.so`) runs in the process space of the calling application (e.g., `sudo`, `gdm`). It behaves strictly as a thin IPC client that transmits the username and receives a boolean success/fail response. This isolates the heavy OpenCV and ONNX runtime dependencies (which have a large codebase and complex parsing attack surface) to the root `linuxcampamd` daemon.
*   **User Space vs. Root Space:** The IPC socket bridges the user space calling process and the root daemon (`linuxcampamd`).
*   **Software vs. Hardware:** The daemon trusts the `V4L2` kernel subsystem for video frame delivery.

## 3. Threat Modeling (STRIDE Methodology)

We use the STRIDE framework to analyze potential threats to the authentication pipeline.

### 3.1 Spoofing (Presentation Attacks)
*   **Threat:** An attacker presents a photo, video, or 3D mask to the camera to impersonate an enrolled user.
*   **Mitigation:** 
    *   **Dual Camera (IR + RGB):** High resistance. Validates overall frame brightness to ensure active IR illumination. Screens and standard photos appear black/dark in near-IR, effectively acting as a liveness check. Note that this relies on 2D IR reflection variance rather than enterprise-grade 3D depth mapping (structured light).
    *   **Single IR:** Medium-High resistance. Vulnerable to specific IR-reflective printed masks.
    *   **Single RGB:** Low resistance (VULNERABLE). Easily spoofed by a high-resolution video on a smartphone. *Not recommended for `sudo`.*

### 3.2 Tampering
*   **Threat 1 (Data):** An attacker with local access modifies the face embeddings (`/etc/linuxcampam/users/*.json`) or the ONNX AI models to inject their own face or backdoor the recognition.
*   **Mitigation:** The `/etc/linuxcampam/users/` directory is secured with `0700` permissions (root-only). 
*   **Residual Risk:** If an attacker already has `root` access, they can modify these files. The system lacks hardware-backed cryptographic binding (like a TPM) which would tightly couple biometric data to specific hardware. (Future roadmap: HMAC integrity checks).

### 3.3 Repudiation
*   **Threat:** A user claims they did not authenticate a `sudo` action.
*   **Mitigation:** The daemon logs authentication successes and failures (via syslog/journald).

### 3.4 Information Disclosure
*   **Threat 1 (Biometric Theft & Replay):** Stealing face embeddings to clone an identity or replay authentication on another system.
*   **Mitigation:** Embeddings are 128-dimensional mathematical abstractions, not raw photos. Stored in root-owned directories (`0700` parent, `0600` files). Raw images are *never* saved unless explicitly enabled for debugging by an administrator.
*   **Residual Risk:** If an attacker achieves root access, they can copy the embedding files and use them on another machine running LinuxCamPAM. (Future roadmap: Cryptographic binding/encryption of embeddings).
*   **Threat 2 (Socket Snooping):** The IPC socket (`/run/linuxcampam/socket`) has `0666` permissions.
*   **Mitigation:** The socket only transmits usernames and boolean results (`AUTH_SUCCESS`/`AUTH_FAIL`). No biometric data or video frames traverse the socket.
*   **Threat 3 (USB/V4L2 Bus Interception):** An attacker with physical access uses a hardware tap to intercept or inject raw video frames over the USB bus.
*   **Mitigation:** Physical security of the hardware. The software inherently trusts the `V4L2` video stream. Unlike some enterprise biometric setups, generic webcams do not provide encrypted sensor links to the OS.
    *   **Active Liveness Check**: Active liveness detection (challenge-response) provides a strong defense against frame injection by requiring unpredictable real-time user reactions, preventing the replay of static or looped video frames (see [Roadmap #6](#5-future-security-enhancements-roadmap)).
    *   **System-Level Recommendation**: For highly sensitive environments, administrators are strongly advised to deploy `usbguard` to block unauthorized USB devices from enumerating, preventing hot-plug camera swap attacks.

### 3.5 Denial of Service (DoS)
*   **Threat:** An attacker spams the socket with authentication requests to exhaust system resources or lock the camera.
*   **Mitigation:** 
    *   **Timeouts:** Hard timeouts (default 3s) on authentication requests.
    *   **Rate Limiting / Lockout:** Configurable lockout after *N* failed attempts (e.g., 5 failures triggers a 5-minute lockout). State is tracked in-memory.

### 3.6 Elevation of Privilege
*   **Threat 1 (Daemon Exploitation):** Exploiting a vulnerability (e.g., buffer overflow) in the root-level `linuxcampamd` daemon via the user-accessible IPC socket.
*   **Mitigation:** 
    *   Strict input sanitization on the socket (usernames must match `^[a-zA-Z0-9_\.-]+$`) preventing Path Traversal.
    *   Use of modern C++ RAII patterns to prevent memory leaks and buffer overflows.
    *   Compiler hardening (PIE, Stack Canaries).

*   **Threat 2 (Unauthorized IPC Command Execution):** The IPC socket (`/run/linuxcampam/socket`) is world-writable (`0666`) to allow unprivileged local users to request authentication. An attacker can connect directly to the socket and issue administrative commands (e.g., `REMOVE_EMBEDDING`, `TRAIN_USER`, `SET_LOG_LEVEL`) to tamper with or disable authentication for other users.
*   **Mitigation:** The CLI client verifies privileges locally (e.g., requiring root/sudo for multi-user operations), but the daemon does not currently enforce peer credential ownership at the Unix socket layer.
*   **Residual Risk:** High risk of local denial of service (deletion of face embeddings) or unauthorized enrollment if an attacker has shell access to the machine.

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
| **Spoofing** | RGB Photo/Video Replay | High (if RGB used) | High | **Critical** | Partially | Warn users against RGB-only setups. Defaults to IR if exists. (See [Roadmap #6](#5-future-security-enhancements-roadmap)) |
| **Spoofing** | Advanced 3D/Heated Mask | Low | High | **Medium** | Partially | IR Liveness detection provides baseline defense. |
| **Tampering** | Root User Modifies Embeddings | Low | High | **Medium** | WiP | Relies on OS boundaries; attacker already has root. (HMAC/encryption planned, see [Roadmap #1](#5-future-security-enhancements-roadmap), [#8](#5-future-security-enhancements-roadmap)) |
| **Tampering** | Evil Maid (USB Camera Swap) | Low | High | **Medium** | Partially | Hardware trust issue. Mitigate via BIOS passwords. Device ID verification (VID/PID/Serial) provides partial mitigation (see [Roadmap #5](#5-future-security-enhancements-roadmap)). |
| **Tampering** | USB Bus Frame Injection | Low | High | **Medium** | No | No encrypted sensor links. Device ID checks do not mitigate bus taps. Challenge-response mitigates pre-recorded frame loops (see [Roadmap #6](#5-future-security-enhancements-roadmap)). Recommend `usbguard` for physical port control. |
| **Info Disclosure**| Read Socket Traffic | Medium | Low | **Low** | Yes | Socket only sends booleans and usernames. |
| **Info Disclosure**| Biometric Embedding Theft | Low | High | **Medium** | Partially | Secured by root-only filesystem permissions. (Encryption planned, see [Roadmap #8](#5-future-security-enhancements-roadmap)) |
| **DoS** | Auth Request Spamming | Low | Medium | **Low** | Yes | Rate limiting and hard timeouts implemented. |
| **Elevation** | Buffer Overflow in Daemon | Low | Critical | **Medium** | Yes | Modern C++, sanitizers, strict socket parsing. |
| **Elevation** | Unauthorized IPC Commands | Medium | High | **High** | No | Lacks socket peer credential verification (`SO_PEERCRED`) in daemon. (Verification planned, see [Roadmap #7](#5-future-security-enhancements-roadmap)) |

## 5. Future Security Enhancements (Roadmap)

To further harden the LinuxCamPAM architecture, the following enhancements are planned:

1.  **Embedding Integrity (HMAC):** Implement cryptographic signatures on user embedding files to detect unauthorized tampering.
2.  **Model Verification:** Enforce SHA256 hash verification of ONNX model files upon load to prevent the injection of backdoored models.
3.  **SELinux / AppArmor Profiles:** Provide strict Mandatory Access Control (MAC) policies confining the daemon strictly to required devices (`/dev/video*`) and sockets, neutralizing potential privilege escalation.
4.  **Configurable Logging Restrictions:** Allow administrators to disable the logging of usernames in production to prevent identity leakage via syslog.
5.  **Hardware ID Verification:** Validate USB Vendor ID (VID), Product ID (PID), and unique Serial Number (where supported by UVC descriptors) to detect and block unauthorized camera hardware swaps.
6.  **Active Liveness Detection (Challenge-Response):** Implement randomized user prompts (e.g., "blink", "turn head") to thwart video replay injection attacks by forcing unpredictable live interaction.
7.  **IPC Socket Peer Verification:** Implement `SO_PEERCRED` checks in the daemon to verify that only root or the corresponding target user can issue administrative commands.
8.  **Embedding Encryption & Hardware Binding:** Encrypt user embedding files locally using a host key or hardware-backed key (via TPM) to protect them from offline extraction and relocation.
