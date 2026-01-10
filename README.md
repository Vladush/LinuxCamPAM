# LinuxCamPAM: Face Authentication for Linux

[![CI](https://github.com/Vladush/LinuxCamPAM/actions/workflows/ci.yml/badge.svg)](https://github.com/Vladush/LinuxCamPAM/actions/workflows/ci.yml)

> Face unlock for Linux, like Windows Hello™. Any webcam works; IR recommended for best results.

LinuxCamPAM provides seamless face unlock for Linux `sudo`, `login`, and login/lock screens (GDM, SDDM, LightDM) using OpenCV and AI models (YuNet/SFace). I built it to solve my own need for speed and reliability, supporting hardware acceleration (OpenCL, CUDA) and smart dual-camera configurations (IR + RGB). Virtually any USB webcam works out of the box.

## Motivation

I built this to solve a real problem for my family. My daughter's Fujitsu T900 has a Swiss-French-German external keyboard plus a German internal one, and she switches between Russian and Ukrainian layouts. Typing passwords on the wrong layout = locked accounts. Face auth fixes that. Also for myself :-)

Since open-sourcing, I've put effort into making it hardware-agnostic and well-documented.

## Features

- **Any Webcam Works**: Standard USB or integrated cameras (720p+ recommended). No special hardware required.
- **Enhanced IR Support**: Detects IR cameras and controls emitters for better low-light and anti-spoofing.
- **Hardware Acceleration**: OpenCL (AMD via Rusticl, Intel, NVIDIA) with optional CUDA backend.
  - **Smart GPU Detection**: Auto-detects AMD GPUs and uses Rusticl. Non-AMD systems use native drivers; falls back to CPU if needed.
- **Smart Camera Support**:
  - **Dual-Camera Security**: Uses IR for liveness/security and RGB for validation.
  - **Auto-Configuration**: Detects your hardware (IR vs RGB) and selects the best policy.
  - **Enhanced Enrollment**: HDR capture when supported, frame averaging for all cameras.
- **Multi-Embedding Support**: Store multiple face embeddings per user for different lighting (`linuxcampam list`, `train --new`).
- **PAM Integration**: Standard PAM module for Debian/Ubuntu.
- **Security First**: [Threat Model & Security Assessment](docs/SECURITY_ASSESSMENT.md) included.

## Installation

### Dependencies

| Package | Purpose |
| --------- | ------- |
| `libopencv-dev` | Face detection (YuNet), recognition (SFace), camera capture, OpenCL acceleration |
| `libpam0g-dev` | PAM module development |
| `nlohmann-json3-dev` | JSON serialization (user embeddings, config) - vendored fallback included |
| `v4l-utils` | Camera detection and diagnostics |
| `cmake`, `build-essential` | Build system |
| `ninja-build` *(optional)* | Faster builds (recommended) |

> **Build System Note:** The install scripts use `make` by default. If you have `ninja-build` installed, you can use Ninja for faster incremental builds:
>
> ```bash
> cmake -G Ninja ..
> ninja
> ```

**Compiler Compatibility:** The project is C++17 compliant and has been rigorously verified on **Ubuntu 24.04** using the following compilers, ensuring compatibility with older distributions (LTS):

| Architecture | Compiler | Status | Notes |
| :--- | :--- | :--- | :--- |
| **x86_64** | GCC 11/12/13 | ✅ Verified | Target: Ubuntu 22.04+ / Debian 12+ |
| **x86_64** | Clang 16/17/18 | ✅ Verified | Target: Ubuntu 22.04+ / Debian 12+ |
| **AARCH64** | GCC (Cross) | ✅ Verified | via Docker/QEMU |
| **RISC-V** | GCC (Cross) | ✅ Verified | via Docker/QEMU |

### Option A: Direct System Install (Quickest)

```bash
# 1. Install Dependencies
./scripts/install_deps.sh

# 2. Build & Install
./scripts/install.sh
```

This compiles the code and installs binaries directly to `/usr/local/bin`. It also:

- **Backs up** your existing PAM configuration.
- **Auto-configures** your cameras.
- **Enables** the module automatically.

> [!NOTE]
> **Silent Installation:** This script is designed to be fully automated and **non-interactive**. It will not ask for confirmation before backing up files or enabling the module. This is by design to support automated deployment (Ansible, Docker, etc.).

### Option B: Build Debian Package (.deb)

To build a clean `.deb` package using CPack:

```bash
# 1. Prepare Build
mkdir -p build && cd build
cmake ..

# 2. Generate Package
cpack -G DEB
```

Then install the generated package:

```bash
sudo dpkg -i linuxcampam_*.deb
```

The package installation will automatically backup your PAM config, configure the cameras, and enable the module.

## Configuration

The configuration file is located at `/etc/linuxcampam/config.ini`.

The installer runs a smart detection script (`linuxcampam-setup-config`) which attempts to auto-configure your cameras. You can re-run this at any time:

```bash
sudo linuxcampam-setup-config
```

> **Note regarding IR Cameras:** If your IR camera does not light up, you likely need to configure the emitter. This project relies on the excellent **[linux-enable-ir-emitter](https://github.com/EmixamPP/linux-enable-ir-emitter)** tool for this. A helper script is included at `scripts/install_ir_emitter.sh` to install it for you.

For advanced multi-camera policies (e.g., Mandatory IR + Optional RGB), see [docs/CONFIGURATION.md](docs/CONFIGURATION.md).

## Usage

**Enroll a User:**

```bash
sudo linuxcampam add <username>
```

**Train (Update) User Model:**

Updates the existing user model with new face data to improve recognition accuracy.

```bash
sudo linuxcampam train <username>
```

**Test Authentication (Diagnostics):**

Performs a hardware check (verifies camera capture) AND attempts to authenticate the current user. Shows detailed errors for debugging.

```bash
linuxcampam test
# Output: HW_OK | AUTH_SUCCESS
# Or:     HW_OK | AUTH_FAIL: No face detected
```

To test a specific user (requires `sudo` for security - prevents user enumeration):

```bash
sudo linuxcampam test <username>
sudo linuxcampam test <username>
# Output: HW_OK | AUTH_FAIL: User not enrolled
```

**Check Version:**

Shows the version of both the CLI client and the running daemon service.

```bash
linuxcampam version
# Output:
# Client Version: 0.9.6
# Daemon Version: 0.9.6
```

## Debugging

If you encounter issues, you can enable verbose debug logging dynamically without restarting the service.

**Enable Debug Logging:**

```bash
sudo linuxcampam debug on
```

**Disable Debug Logging:**

```bash
sudo linuxcampam debug off
```

**Check Status:**

```bash
sudo linuxcampam debug
```

Logs are typically written to systemd journal. You can view them with:

```bash
journalctl -u linuxcampam -f
```

## Credits & Acknowledgements

This project was inspired by and utilizes tools from the open source community:

### Core Libraries

- **[OpenCV](https://opencv.org/)**: The heart of this project. Used for camera capture, image processing, DNN inference (YuNet/SFace), and OpenCL hardware acceleration.
- **[nlohmann/json](https://github.com/nlohmann/json)**: The de facto standard JSON library for C++. Used for config and user data serialization. Vendored as header-only.

### AI Models

- **[OpenCV Zoo](https://github.com/opencv/opencv_zoo)**: Pre-trained ONNX models (YuNet face detection, SFace recognition).

### Tools & Inspiration

- **[linux-enable-ir-emitter](https://github.com/EmixamPP/linux-enable-ir-emitter)**: By [EmixamPP](https://github.com/EmixamPP). Essential for enabling IR emitters on many Linux laptops.
- **[Howdy](https://github.com/boltgolt/howdy)**: The pioneer of Windows Hello-style authentication on Linux, serving as inspiration for the user experience.

## Roadmap

- [ ] **NPU Support:** Integrate AMD Ryzen AI / XDNA for power-efficient inference (pending AMD Ryzen AI SDK availability for Linux).
- [ ] **Liveness Detection:**
  - *Passive:* Frame variance analysis during enrollment (static images = spoof attempt).
  - *Active:* Challenges like blink, smile, head turn to prevent video replay attacks.
- [x] **Rate Limiting:** Per-user lockout after failed auth attempts (configurable via `[Security]` in config.ini).
- [ ] **Security Hardening:** Embedding integrity (HMAC), model file verification, and configurable logging.
- [ ] **Adaptive Enrollment** *(needs investigation)*: Auto-update embeddings when face auth fails but password succeeds. Requires careful security analysis.
- [ ] **GUI Config Tool:** A simple GTK/Qt app for managing users and cameras.
- [ ] **Enterprise Features:**
  - Embedding export/import with model version validation for cross-machine portability.
  - Remote backend support (LDAP, REST API, RADIUS-style) for centralized user management.
  - *Security:* TLS for transit, nonce-based replay protection, local caching with TTL.

## Contributing & Support

I am excited to see this project grow beyond the hardware I currently possess! **I am actively accepting Pull Requests, Feature Requests, and Bug Reports.**

- **Porting:** If you are on a Linux distribution such as [Gentoo](https://www.gentoo.org/), [Calculate](https://www.calculate-linux.org/), [Arch](https://archlinux.org/), [Fedora](https://fedoraproject.org/), or another, and want help adapting the packaging, open an issue! I am happy to help guide the process as my time permits.
- **Hardware Support:** I am open to supporting different camera configurations and hardware quirks. If you have unique hardware, feel free to report issues or suggest improvements.
- **Discussion:** Ideas for new features (like the Liveness Detection above) are welcome.
- **Time Commitment:** Please note this is a personal project. While I strive to be responsive, my availability for support depends on my free time.

## Security & Permissions

> [!IMPORTANT]
> **Why is `sudo` required for CLI commands?**
>
> All CLI commands (`add`, `train`, `test`) require `sudo`. This is a deliberate security feature.
>
> - **Protected Data**: Face embeddings are stored in `/etc/linuxcampam/users`, which is writable only by root.
> - **Socket Access**: The socket `/run/linuxcampam/socket` is world-accessible (`0666`) so PAM modules can connect during login.
>
> **Does this affect normal users?**
> **No.** Standard users can log in using face unlock because login managers (`gdm`, `login`, `sudo`) invoke PAM which connects to the service. Only *management* tasks require root.

## License

MIT License

---

*Windows Hello is a trademark of Microsoft Corporation. This project is not affiliated with or endorsed by Microsoft.*
