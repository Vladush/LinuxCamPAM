# LinuxCamPAM: Face Authentication for Linux

[![CI](https://github.com/Vladush/LinuxCamPAM/actions/workflows/ci.yml/badge.svg)](https://github.com/Vladush/LinuxCamPAM/actions/workflows/ci.yml)

> Face unlock for Linux, like Windows Hello™. Any webcam works; IR recommended for best results.

LinuxCamPAM provides seamless face unlock for Linux `sudo`, `login`, and login/lock screens (GDM, SDDM, LightDM) using OpenCV and AI models (YuNet/SFace). I built it to solve my own need for speed and reliability, supporting hardware acceleration (OpenCL) and smart dual-camera configurations (IR + RGB). Virtually any USB webcam works out of the box.

## Why This Exists Now

There are existing tools like `howdy`, but they often suffer from being slow, lacking hardware acceleration, or failing completely with modern Python environments and D-Bus integration under Wayland.

- **Why OpenCV C++**: It runs instantly. No lag when typing `sudo`.
- **Why ONNX + YuNet + SFace**: Replaces the sluggish `dlib` backend for state-of-the-art face detection and recognition.
- **Native Backends**: Native OpenVINO is not enabled in the default static build to maintain a small package size (~15MB vs ~300MB+). OpenCL provides near-native performance for this use case without the bloat.

## Motivation

I built this to solve a real problem for my family. My daughter's Fujitsu T900 has a Swiss-French-German external keyboard plus a German internal one, and she switches between Russian and Ukrainian layouts. Typing passwords on the wrong layout = locked accounts. Face auth fixes that. Also for myself :-)

Since open-sourcing, I've put effort into making it hardware-agnostic and well-documented.

## Features

- **Any Webcam Works**: Standard USB or integrated cameras (720p+ recommended). No special hardware required.
- **Enhanced IR Support**: Detects IR cameras and controls emitters for better low-light and anti-spoofing.
- **Hardware Acceleration**:
  - **Strategy**: **OpenCL** is prioritized as the universal accelerator. It enables GPU acceleration on Intel (iGPU), NVIDIA (proprietary driver), AMD (ROCm/Rusticl), and ARM (Mali/Adreno) with a single lightweight package.
  - **Native Backends**: Native CUDA and OpenVINO are not enabled in the default static build to maintain a small package size (~15MB vs ~300MB+). OpenCL provides near-native performance for this use case without the bloat.
  - **Smart GPU Detection**: Auto-detects available acceleration; falls back to optimized CPU instructions (AVX/SSE) if no GPU is available.
- **Smart Camera Support**:
  - **Dual-Camera Security**: Uses IR for liveness/security and RGB for validation.
  - **Auto-Configuration**: Detects your hardware (IR vs RGB) and selects the best policy.
  - **Enhanced Enrollment**: HDR capture when supported, frame averaging for all cameras.
- **Proximity Sensor Integration**: Native support for I2C HID human presence sensors (e.g., ITE8353) to detect human presence, optimize authentication, and reduce idle processing.
- **Zero-Interaction Login**: Automatically wakes your displays and logs you in the moment you sit down, then automatically locks the OS when you walk away. (See [Threat Model](docs/THREAT_MODEL_AND_RISK_ASSESSMENT.md) for related security considerations, including walk-by unlocks and physical coercion).
- **Universal Confirmation & Password Fallback**: Prevent silent privilege escalation via active user intent confirmation (`<Enter>`) for sensitive PAM actions like `sudo` or `su`, with an instant bypass for legacy passwords.
- **Multi-Embedding Support**: Store multiple face embeddings per user for different lighting (`linuxcampam list`, `train --new`).
- **PAM Integration**: Standard PAM module for Debian/Ubuntu.
- **Security First**: [Threat Model & Risk Assessment](docs/THREAT_MODEL_AND_RISK_ASSESSMENT.md) included.

## System Requirements

| Component        | Minimum Requirement                | Recommended                                     |
| :--------------- | :--------------------------------- | :---------------------------------------------- |
| **OS**           | Linux (Kernel 5.15+)               | Linux (Kernel 6.1+)                             |
| **Distribution** | Ubuntu 18.04 LTS (GLIBC 2.27+) [1] | Ubuntu 24.04 LTS / Debian 12                    |
| **CPU**          | x86_64, x86, aarch64, riscv64      | x86_64 (AVX2), aarch64 (NEON), riscv64 (Vector) |
| **RAM**          | 256MB free                         | 512MB free                                      |
| **Camera**       | V4L2-compatible (360p)             | IR + RGB (720p)                                 |
| **Access**       | Root (Daemon) / User (CLI)         | Root (Daemon) / User (CLI)                      |

> [1] **Legacy Distros (Ubuntu 18.04 / Debian 10)**: The default `cmake` is too old (< 3.14). You must upgrade to **CMake 3.14+** (e.g., via `pip install cmake`) to build this project. OpenCV 4.12 itself builds fine.

## Installation

### Option A: Install from Pre-Build Debian Package (Recommended)

If you have downloaded a release file (`.deb`) from the [latest release](https://github.com/Vladush/LinuxCamPAM/releases/latest), installation is extremely simple:

```bash
sudo apt install ./linuxcampam_*.deb
```

That's it! `apt` handles all dependencies for you.

### Option B: Build from Source (Quickest System Build)

1. **Install Build Tools**:

   ```bash
   ./scripts/install_deps.sh
   ```

2. **Compile Dependencies (Static OpenCV)**:
   This takes ~15-20 minutes but ensures a stable, portable build.

   ```bash
   ./scripts/build_opencv.sh
   ```

3. **Build & Install Project**:

   ```bash
   ./scripts/install.sh [--lock]
   ```

This compiles the code and installs binaries to `/usr/local/bin`. It also:

- **Backs up** your existing PAM configuration.
- **Auto-configures** your cameras.
- **Enables** the module automatically.

> [!NOTE]
> **Silent Installation:** This script is designed to be fully automated and **non-interactive**. It will not ask for confirmation before backing up files or enabling the module.
>
> **Configuration Lock:** The setup script provides an interactive prompt (or `--lock` flag) to lock your configuration (`chattr +i`) for enhanced security. If you opt into this, you must unlock it before editing it manually: `sudo chattr -i /etc/linuxcampam/config.ini`.

### Option C: Build Debian Package (.deb)

To build a clean `.deb` package using CPack:

```bash
# 1. Build Static Dependencies
./scripts/build_opencv.sh

# 2. Prepare Build
mkdir -p build && cd build
cmake ..

# 3. Generate Package
cpack -G DEB
```

### Build Options (CMake)

When building from source, you can customize the compilation by passing variables to `cmake`.

| CMake Option | Default | Description |
| :--- | :--- | :--- |
| `-DDISABLE_WELCOME_MESSAGE=ON` | `OFF` | Completely strips the welcome message logic from the compiled PAM module and executable. This provides maximum stealth and security, ignoring any INI or PAM arguments. |

Then install the generated package:

```bash
sudo apt install ./linuxcampam_*.deb
```

The package installation will automatically backup your PAM config, configure the cameras, and enable the module.

> [!TIP]
> **Visual Guide**: detailed flowcharts for installation and enrollment are available in the [User Flows Guide](docs/USER_FLOWS.md).

### Build Dependencies & Compatibility

#### Build Dependencies

> **Don't panic!** You don't need to manually install these. The provided script `scripts/install_deps.sh` will automatically install everything you need. This list is just for reference or for those building manually.

These are required to **compile** the project from source (Options B & C).

| Package                      | Purpose                                     |
| :--------------------------- | :------------------------------------------ |
| `cmake`, `build-essential`   | Build system                                |
| `libpam0g-dev`               | PAM module development headers              |
| `libudev-dev`                | Required for HIDAPI proximity sensor [2]    |
| `v4l-utils`                  | Camera detection tools (also a runtime dep) |
| `curl` / `wget`              | Downloading models and dependencies         |
| `ninja-build` *(optional)*   | Faster builds (recommended)                 |

> [2] **libudev-dev Troubleshooting**: This is a hard dependency for `hidapi` (via `pkg-config`). On Ubuntu/Debian, the package is `libudev-dev`. On RedHat/Fedora/CentOS-based distributions, it is typically named `systemd-devel` or `libudev-devel`. If CMake fails on `PkgConfig::HIDAPI`, ensure these headers are installed.
>
> **Runtime Dependencies**: If you are installing a pre-built `.deb` package, the package manager (`apt`/`dpkg`) will automatically install the necessary runtime libraries (e.g., `libpam0g`, `libatlas3-base`). You do **not** need the `-dev` development headers for running the software.
>
> **Build System Note:** The install scripts use `make` by default. If you have `ninja-build` installed, you can use Ninja for faster incremental builds:
>
> ```bash
> cmake -G Ninja ..
> ninja
> ```

#### Compiler Compatibility

The project is C++17 compliant. While strictly verified on **Ubuntu 24.04**, the codebase includes polyfills (e.g., `<charconv>` fallbacks) to support older compilers found in **Ubuntu 20.04 LTS (GCC 9)** and Debian 11.

| Architecture | Compiler    | Status        | Notes                        |
| :----------- | :---------- | :------------ | :--------------------------- |
| **x86_64**   | GCC 9+      | ✅ Compatible | Ubuntu 20.04+ / Debian 11+   |
| **x86_64**   | GCC 11+     | ✅ Verified   | Ubuntu 22.04+ / Debian 12+   |
| **x86_64**   | Clang 10+   | ✅ Compatible | Ubuntu 20.04+                |
| **AARCH64**  | GCC (Cross) | ✅ Verified   | via Docker/QEMU              |
| **RISC-V**   | GCC (Cross) | ✅ Verified   | via Docker/QEMU              |

> **Note**: A **static version of OpenCV 4.12.0** is now compiled automatically. You do **not** need to install `libopencv-dev` or generic system libraries anymore. This ensures the authentication service runs reliably regardless of your OS version.

## Configuration

The configuration file is at `/etc/linuxcampam/config.ini`.

> **Need help choosing a config?** Check the [Configuration Decision Tree](docs/USER_FLOWS.md#2-configuration-helper-which-setup-is-right-for-me).

The installer runs a smart detection script (`linuxcampam-setup-config`) to auto-configure your cameras. You can re-run this at any time:

```bash
sudo linuxcampam-setup-config
```

*(Note: To lock the configuration non-interactively in automated deployments, append the `--lock` flag).*

> **IR Camera Note:** If your IR camera implies it's working but doesn't light up, you likely need to configure the emitter. This project relies on the excellent **[linux-enable-ir-emitter](https://github.com/EmixamPP/linux-enable-ir-emitter)** tool for this. Run `scripts/install_ir_emitter.sh` to install it (it will use a patched version from <https://github.com/Vladush/linux-enable-ir-emitter> by default).

For advanced policies (e.g., Mandatory IR + Optional RGB), see the [Configuration Guide](docs/CONFIGURATION.md).

## Usage

**Enroll a User:**

```bash
linuxcampam add <username>
```

**Train (Update) User Model:**

Updates the existing user model with new face data to improve recognition accuracy.

```bash
linuxcampam train <username>
```

**Test Authentication (Diagnostics):**

Performs a hardware check and attempts to authenticate the current user. Shows detailed errors for debugging.

```bash
linuxcampam test
# Output: HW_OK | AUTH_SUCCESS
# Or:     HW_OK | AUTH_FAIL: No face detected
```

To test a specific user (requires `sudo` for security):

```bash
sudo linuxcampam test <username>
# Output: HW_OK | AUTH_FAIL: User not enrolled
```

**Check Version:**

Shows the version of both the CLI client and the running daemon service.

```bash
linuxcampam version
# Output:
# Client Version: 0.9.7.5+ga8f02f6
# Daemon Version: 0.9.7.5+ga8f02f6
```

## Debugging

If you encounter issues, start with the **[Troubleshooting Wizard](docs/USER_FLOWS.md#1-troubleshooting-wizard-why-is-it-failing)**.

If you are using an IR camera and it fails to enroll or authenticate, see the **[IR Camera Troubleshooting Guide](docs/IR_CAMERA_TROUBLESHOOTING.md)**.

If you need deeper logs, you can enable verbose debug logging dynamically without restarting the service.

**Enable Debug Logging:**

```bash
linuxcampam debug on
```

**Disable Debug Logging:**

```bash
linuxcampam debug off
```

**Check Status:**

```bash
linuxcampam debug
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

- **[linux-enable-ir-emitter](https://github.com/EmixamPP/linux-enable-ir-emitter)**: By [EmixamPP](https://github.com/EmixamPP). Essential for enabling IR emitters on many Linux laptops. (Note: A patched version 7 with refined UI controls is available at <https://github.com/Vladush/linux-enable-ir-emitter>).
- **[Howdy](https://github.com/boltgolt/howdy)**: The pioneer of Windows Hello-style authentication on Linux, serving as inspiration for the user experience.

### Citations

If you use the YuNet model included in this project for research, please cite the following paper:

```bibtex
@article{wu2023yunet,
  title={Yunet: A tiny millisecond-level face detector},
  author={Wu, Wei and Peng, Hanyang and Yu, Shiqi},
  journal={Machine Intelligence Research},
  volume={20},
  number={5},
  pages={656--665},
  year={2023},
  publisher={Springer}
}
```

## Roadmap

- [ ] **NPU Support:** Integrate AMD Ryzen AI / XDNA for power-efficient inference (pending AMD Ryzen AI SDK availability for Linux).
- [ ] **Liveness Detection:**
  - *Passive:* Frame variance analysis during enrollment (static images = spoof attempt).
  - *Active:* Challenges like blink, smile, head turn to prevent video replay attacks.
- [x] **Rate Limiting:** Per-user lockout after failed auth attempts (configurable via `[Security]` in config.ini).
- [x] **Socket Security:** IPC Socket peer credential verification (`SO_PEERCRED`) to prevent DoS attacks.
- [ ] **Security Hardening:** Embedding integrity (HMAC), model file verification, and configurable logging.
- [ ] **Privilege Separation:** Transition the daemon from running as `root` to a dedicated `linuxcampam` service user.
- [ ] **Adaptive Enrollment** *(needs investigation)*: Auto-update embeddings when face auth fails but password succeeds. Requires careful security analysis.
- [ ] **GUI Config Tool:** A simple GTK/Qt app for managing users and cameras.
- [ ] **D-Bus State Synchronization:** Native DBus listener for `org.freedesktop.login1.Session` to accurately synchronize the daemon's internal state with manual OS-level locks.
- [ ] **Enterprise Features:**
  - Embedding export/import with model version validation for cross-machine portability.
  - Remote backend support (LDAP, REST API, RADIUS-style) for centralized user management.
  - *Security:* TLS for transit, nonce-based replay protection, local caching with TTL.

## Contributing & Support

I am excited to see this project grow beyond the hardware I currently possess! **I am actively accepting Pull Requests, Feature Requests, and Bug Reports.**

New contributors should read the **[Architecture Documentation](docs/ARCHITECTURE.md)** for a technical overview of the system.

- **Porting:** If you are on a Linux distribution such as [Gentoo](https://www.gentoo.org/), [Calculate](https://www.calculate-linux.org/), [Arch](https://archlinux.org/), [Fedora](https://fedoraproject.org/), or another, and want help adapting the packaging, open an issue! I am happy to help guide the process as my time permits.
- **Hardware Support:** I am open to supporting different camera configurations and hardware quirks. If you have unique hardware, feel free to report issues or suggest improvements.
- **Discussion:** Ideas for new features (like the Liveness Detection above) are welcome.
- **Time Commitment:** Please note this is a personal project. While I strive to be responsive, my availability for support depends on my free time.

## Security & Threat Model

LinuxCamPAM takes system security seriously. A comprehensive breakdown of the protected assets, trust boundaries, STRIDE threat modeling, and mitigation strategies (including hardware swaps, frame injection, and `config.ini` tampering) is available here:

👉 **[Read the Threat Model & Risk Assessment](docs/THREAT_MODEL_AND_RISK_ASSESSMENT.md)** 👈

For information on how to securely report vulnerabilities, please see our **[Security Policy](SECURITY.md)**.

> [!IMPORTANT]
> **Permissions & Sudo Usage**
>
> The `linuxcampamd` service runs as `root` to securely manage face data in `/etc/linuxcampam/users`. The CLI tool communicates with this service via a socket.
>
> - **Standard Users**: Can enroll, train, and test their *own* face data **without `sudo`**.
> - **Administration**: Functional tests of *other* users (e.g., `linuxcampam test otheruser`) will enforce `sudo` usage for security.
> - **Service Management**: Commands that restart the service or edit config files manually (like `linuxcampam-setup-config`) still require `sudo`.
> - **System Users**: By default, users with UID < 1000 (like `sddm`, `gdm`) are ignored to prevent boot conflicts. This is configurable in `config.ini`.

## License

MIT License

---

*Windows Hello is a trademark of Microsoft Corporation. This project is not affiliated with or endorsed by Microsoft.*
