# Security Assessment & Threat Model

This document breaks down the security design of **LinuxCamPAM**. It explains what we protect against, where the system is vulnerable, and the trade-offs we made.

> **Reality Check:** Biometrics are always a trade-off between convenience and security. No face recognition system is perfect. LinuxCamPAM is built to secure your personal laptop, not a nuclear silo.

## 1. Threat Model

### 1.1 Assets

* **System Access:** `sudo` privileges, login sessions, desktop unlocking.
* **Biometric Data:** Face embeddings stored in `/etc/linuxcampam/users/`.
* **Privacy:** Camera feed privacy (no unauthorized surveillance).

### 1.2 Attack Vectors

* **Presentation Attacks (Spoofing):** Attempting to assume a user's identity by presenting a photo, video, or mask to the camera.
* **Replay Attacks:** Injecting pre-recorded camera frames into the authentication pipeline.
* **Privilege Escalation:** Exploiting the background service (`linuxcampamd`) to gain root access.
* **Data Theft:** Stealing biometric templates to reverse-engineer or clone an identity.

---

## 2. Hardware Security (Camera Configurations)

The security of this system heavily depends on the camera hardware used.

### 2.1 Configuration Risk Matrix

| Configuration | Spoofing Resistance | Vulnerability |
| :--- | :--- | :--- |
| **Dual Camera (IR + RGB)** | **High** | Requires a sophisticated 3D mask or heated mask to bypass. Standard photos/videos fail against IR check. |
| **Single IR Camera** | **Medium-High** | Resists standard photos (paper/screens are usually cold/dark in IR). Vulnerable to printed IR-reflective photos or specific materials. |
| **Single RGB Camera** | **Low** | **VULNERABLE.** High-resolution photos or videos on a smartphone screen can easily spoof this. |

> **Recommendation:** It is strongly discouraged to use **Single RGB** mode for anything critical (like `sudo`). It is provided for convenience/testing only.

### 2.2 Liveness Detection

* **IR Implementation:** The system validates `min_brightness` on the IR feed. Since human skin reflects IR differently than paper or screens (which often appear black in near-IR), this acts as a basic liveness check.
* **Limitations:** This is not true "3D Depth" sensing (like Apple FaceID). It is significantly better than a simple webcam but less secure than structured light sensors.

---

## 3. System Security Architecture

### 3.1 Privilege Separation

* **Daemon (`linuxcampamd`):** Runs as `root`. This is required to access `/dev/video*` devices (often restricted), write system logs, and validate against the root-owned user database.
* **Risk:** A compromise of the daemon yields root access.
* **Mitigation:**
  * Codebase is minimal and strictly scoped.
  * Compiler hardening (Stack Canaries, PIE, FORTIFY_SOURCE) is enforced.
  * Use of **C++ RAII** patterns (`std::vector`, `std::unique_ptr`) to automate resource cleanup, preventing common manual memory management errors compared to C.

### 3.2 Inter-Process Communication (IPC)

* **Mechanism:** Unix Domain Socket at `/run/linuxcampam/socket`.
* **Access Control:** The socket permissions are set to `0666` (World Read/Write).

> **Why 0666 for the socket?** While PAM modules typically run as root (inherited from applications like `sudo`, `login`, `gdm`), the socket uses `0666` for IPC accessibility. This is safe because:
>
> * The socket only returns `AUTH_SUCCESS` or `AUTH_FAIL` - no sensitive data is exposed
> * Authentication requires physical presence in front of the camera
> * User embedding data is stored separately with restrictive permissions

#### Security Mitigations

| Mitigation | Description |
| :--- | :--- |
| **No Data Exposure** | Socket only returns `AUTH_SUCCESS` or `AUTH_FAIL` - no face data leaks |
| **Camera is Gatekeeper** | You can't authenticate without being physically in front of the camera |
| **Protected User Data** | Face embeddings in `/etc/linuxcampam/users/` are root-only (`0700` directory, `0600` files) |
| **Root-Only Management** | Only `sudo linuxcampam add/train` can modify user data |

#### Residual Risk

A local attacker could spam authentication requests, causing:

* Camera activation (privacy concern)
* Minor resource consumption

This is low-severity since they'd already have local access.

### 3.3 Data Storage

* **Location:** `/etc/linuxcampam/users/`
* **Permissions:** `0700` (Root access only).
* **Content:** JSON files containing face embeddings (float vectors).
* **Privacy:** Raw images are **never** stored unless `[Storage] save_success_images` or `save_fail_images` is explicitly enabled by the admin for debugging.
* **Reversibility:** Face embeddings are mathematical abstractions. While theoretically possible to reconstruct a "ghost" face from them, they are not actual photographs.

---

## 4. Input Validation & Hardening

### 4.1 Input Sanitization

The service implements strict validation on all inputs (specifically usernames) received via the socket:

* **Constraint:** Usernames must match `^[a-zA-Z0-9_\.-]+$`.
* **Prevention:** This blocks **Path Traversal Attacks** (e.g., `../../etc/passwd`) that could attempt to overwrite arbitrary system files during enrollment.

### 4.2 DoS Protection

* **Timeout:** Authentication requests have a hard timeout (default 3s).
* **Socket:** The service uses a persistent connection model but handles requests sequentially to prevent resource exhaustion.

---

## 5. Residual Risks (What remains?)

1. **"Evil Maid" Attack:** If an attacker has physical access to the machine *and* can manipulate the camera hardware (e.g., plugging in a fake USB webcam that replays a video), they could bypass authentication.
    * *Mitigation:* Secure Boot and BIOS passwords.
2. **Model Bias:** The AI models (YuNet/SFace) may have varying accuracy across different demographics or lighting conditions.
3. **USB Camera Injection:** Linux generally trusts USB devices. A "Rubber Ducky" style device mimicking a webcam could inject frames.
    * *Mitigation:* LinuxCamPAM checks specific device paths, but these can be spoofed if the attacker controls the USB stack.
4. **No Brute-Force Protection:** There is currently no rate limiting. A compromised root process could spam authentication attempts indefinitely.
5. **Embedding Tampering:** If an attacker gains root access, they could modify user embedding files (`/etc/linuxcampam/users/*.json`) to inject their own face. No cryptographic integrity check (e.g., HMAC) is currently performed.
6. **Model Tampering:** Similarly, ONNX model files could be replaced with backdoored versions. No hash verification is performed on load.
7. **Information Leakage via Logging:** Usernames are currently logged to stdout/syslog, which could leak identity information in multi-user or shared environments.

---

## 6. SELinux / MAC Context

If this service is running on a SELinux-enforced system (e.g., Fedora, RHEL, CentOS), the security posture improves significantly against **Tampering** risks.

### 6.1 What SELinux Mitigates

* **Embedding & Model Tampering (Risks #5 & #6):** By defining specific file contexts (e.g., `linuxcampam_var_lib_t`) and a strict policy for the daemon domain (`linuxcampam_t`), you can prevent even the `root` user (or other compromised root services) from modifying biometric data or models, unless they explicitly switch roles/contexts. This effectively neutralizes "God Mode" root exploits from touching authentication data.
* **Privilege Escalation (Risk #1.2):** If the `linuxcampamd` process is compromised (e.g., via buffer overflow), a strict SELinux policy can confine it to *only* reading video devices and writing to its socket, preventing it from touching `/etc/shadow`, installing keyloggers, or persisting malware.

### 6.2 What SELinux Does NOT Mitigate

* **Physical / Input Attacks (Risk #1, #3):** SELinux governs *software access*. It cannot detect if the video feed coming from `/dev/video0` is a real face or a injected HDMI signal from a "Rubber Ducky" device.
* **Logic Flaws (Risk #2, #4):** SELinux cannot fix missing rate-limiting code or algorithmic bias.
* **Info Leakage (Risk #7):** If the application creates a log file, SELinux allows it (because it must). It does not "redact" content.

### 6.3 Hardened Systems (Grsecurity, PaX, Strict Kernel)

If you are running a hardened kernel or strict userspace (e.g., Gentoo Hardened):

* **Memory Protections (PaX/MPROTECT):**
  * LinuxCamPAM is built with **PIE** (Position Independent Executable) and **Relro/Now** binding, making it fully compatible with ASLR and PaX.
  * *Note:* The OpenCV DNN backend may attempt JIT compilation for performance. On systems with strict `MPROTECT` (preventing `WX` memory mappings), this *might* cause crashes unless an exception is granted (via `paxctl` or xattr).
* **Systemd Isolation:**
  * The provided service file is minimal. For ultra-hardened setups, administrators should extend `linuxcampam.service` with overrides:

        ```ini
        [Service]
        ProtectSystem=strict
        PrivateTmp=true
        ProtectHome=true
        DeviceAllow=/dev/video* rwm
        ```

* **Seccomp:** No built-in BPF filter is currently shipped. Hardened setups relying on strict syscall allow-listing will need to profile the daemon (e.g., using `strace`) to generate a custom seccomp profile.

---

## 7. Future Security Enhancements (Roadmap)

The following security features are planned for future releases:

| Feature | Description |
| :--- | :--- |
| **Rate Limiting / Lockout** | Configurable lockout after N failed authentication attempts to mitigate brute-force attacks. |
| **Embedding Integrity (HMAC)** | Cryptographic signature on user embedding files to detect tampering. |
| **Model Verification (SHA256)** | Hash verification of ONNX model files on load to prevent backdoored model injection. |
| **Configurable Logging** | Option to disable verbose logging in production to prevent username leakage via syslog. |
| **Nonce/Challenge-Response (IPC)** | Session-based challenge in the IPC protocol to prevent replay attacks (advanced). |
