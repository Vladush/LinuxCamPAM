# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.9.7.4] - 2026-06-02

### Fixed

- **Code:** Addressed documented invariants and enforced CLI privileges.
- **Docs:** Resolved configuration and documentation discrepancies.
- **CI:** Fixed Docker timeout and updated lint workflows with explicit permissions.

### Added

- **Docs:** Updated installation steps with Pre-Build Debian Package instructions and a link to the latest release.

## [0.9.7.3] - 2026-05-17

### Added

- **Configurable Welcome Message**: Added support to configure or disable the PAM welcome message via module arguments (`disable_welcome`) or config file (`[Auth] welcome_message`). Also added a compile-time option (`DISABLE_WELCOME_MESSAGE`) to disable it entirely.

### Performance & Security

- **Smoother Default experience**: Changed default `gpu_throttle_ms` to 20ms. This small pause between GPU operations prevents the dreaded "UI freeze" on laptops with integrated graphics during authentication.
- **Faster First Login**: AI models are now pre-loaded when the service starts, rather than waiting for the first user to walk by.
- **Hardened Validation**: Switched input validation from regex to a strict character-allowlist loop. This isn't visible to users, but it removes a potential ReDoS attack vector and makes path traversal protection bulletproof.

## [0.9.7.2-2] - 2026-03-29

### Dependencies & Scripts

- **Specialized IR Emitter**: Switched to `feat/tweak-controls` branch of the `linux-enable-ir-emitter` fork for better hardware control management.
- **Robustness**: Improved the IR emitter installation script to intelligently skip pre-built binary checks when using feature branches, avoiding unnecessary API errors.

## [0.9.7.2] - 2026-02-19

### Improved in 0.9.7.2

- **CI Build Performance**: Implemented GHCR-based Docker layer caching and optimized OpenCV build by disabling unused modules (`ml`, `video`, `flann`, etc.), significantly reducing build times for i386 and riscv64.

### Fixed in 0.9.7.2

- **Configuration Parsing**: Fixed parsing issues with `[Auth]` and `[Capture]` sections in `config.ini`.
- **Documentation**: Aligned documentation permissions and configuration examples.
- **Package Update Stability**: Added `preinst` script to archive conflicting manual installations.

## [0.9.7] - 2026-01-25

### Improved in 0.9.7

- **Upgraded AI Model**: Switched to `YuNet 2023mar` for better face detection accuracy, especially with smaller faces and difficult angles.
- **Dynamic Resolution**: Removed fixed resizing (320x320) for face detection. The engine now uses the camera's native resolution, which significantly improves detection performance on IR cameras with non-square aspect ratios.
- **Static Linking**: Upgraded to **OpenCV 4.12.0** (statically linked), removing runtime dependencies and potential conflicts.
- **Hardware Strategy**: Prioritized **OpenCL 1.2+** as the universal accelerator (AMD/Intel/NVIDIA) to keep the package lightweight (~15MB). Native CUDA/OpenVINO backends are disabled to avoid massive binary bloat (~200MB+), with OpenCL providing near-native performance.

## [0.9.6] - 2026-01-07

### Added in 0.9.6

- **Dynamic Debug Logging**: Toggle debug logs at runtime with `linuxcampam debug on/off`.
- **Version Command**: Check version with `linuxcampam version` or `linuxcampamd --version`.

### Fixed in 0.9.6

- Missing systemd service file in Debian package (`lib/systemd/system/linuxcampamd.service`).
- Missing PAM config file in Debian package (`usr/share/pam-configs/linuxcampam`).
- Removed unnecessary `-dev` dependencies (OpenCV, etc.) from runtime package.

## [0.9.5] - 2026-01-06

### Fixed in 0.9.5

- GPU sync option to prevent OpenCL hangs on some AMD GPUs
- Missing i386/riscv64 debs in releases (CI extraction fix)
- Auto-clear OpenCL kernel cache on startup to prevent issues after upgrades

### Changed in 0.9.5

- Added `.dockerignore` for cleaner cross-arch builds

## [0.9.3] - 2026-01-03

### Fixed in 0.9.3

- Fixed Debian package build failure by downloading ONNX models before packaging.
- Added `workflow_dispatch` trigger to Release workflow.

## [0.9.2] - 2025-12-31

### Improved in 0.9.2

- **Portable Camera Detection**: Dynamic V4L2 enumeration replaces hardcoded `/dev/video0` and `/dev/video2` paths
  - Cameras auto-classified as IR or RGB based on pixel format support (GREY/Y8 vs MJPEG/YUYV)
  - No more silent fallbacks to non-existent devices
- **Portable GPU Detection**: `detect_opencl.sh` now uses sysfs (`/sys/bus/pci/devices/`) instead of requiring `lspci`
  - Works in containers, VMs, ARM SBCs, and minimal Linux installations
  - Multi-path Rusticl ICD detection for cross-distro compatibility
- **Better Installation UX**: `setup_config.sh` now fails with clear error if no cameras detected
  - Detailed troubleshooting guidance printed on failure
- **Better IR Emitter Script**: Clear guidance when Rust toolchain not available
  - Suggests version 6.1.2 (no Rust required) or Rust installation steps

### Fixed in 0.9.2

- Installation no longer silently proceeds on camera-less systems
- Removed hardcoded camera path fallbacks in C++ auto-detection logic
- `lspci` no longer required for AMD GPU detection

## [0.9.1] - 2025-12-24

### Added in 0.9.1

- **Multi-Embedding Support**: Store multiple face embeddings per user for different lighting conditions
  - `linuxcampam list <user>` - List embedding labels
  - `linuxcampam remove <user> --label <name>` - Remove specific embedding
  - `linuxcampam train --new` - Add new embedding variant
  - `max_embeddings` config option (default: 5, 0 = unlimited)
- **Enhanced Capture for Enrollment**:
  - HDR capture (multi-exposure merge) for cameras with manual exposure control
  - Frame averaging for all cameras (reduces noise)
  - V4L2 runtime detection of camera capabilities
- **New Config Section `[Capture]`**:
  - `enroll_hdr = auto|on|off` - Control HDR usage
  - `enroll_averaging = on|off` - Enable frame averaging
  - `enroll_average_frames = 5` - Number of frames to average
  - `verify_averaging = off` - Optional averaging during verification
- **Per-Camera Capture Settings**: Override global `[Capture]` settings per camera in `[Camera.xxx]` sections:
  - `enroll_hdr`, `enroll_averaging`, `enroll_average_frames`
- **Detailed Test Errors**: The `test` command now shows specific failure reasons:
  - `AUTH_FAIL: User not enrolled`
  - `AUTH_FAIL: No face detected`
  - `AUTH_FAIL: Face mismatch (score: X.XX)`
- **Model Version Tracking**: Embeddings now include `model_version` field for cross-machine portability validation
- Improved camera warmup (10 frames + 100ms settling delay)

### Improved in 0.9.1

- **Overwrite Confirmation**: CLI now prompts before overwriting existing embeddings
- Test command runs single capture cycle (was double, causing IR timing issues)

### Security in 0.9.1

- `linuxcampam test <other_user>` now requires sudo (prevents user enumeration)
- PAM auth responses remain generic (only test command shows detailed errors)

### Fixed in 0.9.1

- Camera capture returning black frames due to insufficient warmup
- Test command returning AUTH_FAIL due to double IR trigger
- Build `-pie` warning for shared libraries
- Train command not reading existing user JSON

## [0.9.0] - 2025-12-18

### Added in 0.9.0

- Initial public release
- PAM module for face authentication (`pam_linuxcampam.so`)
- Background service daemon (`linuxcampamd`)
- CLI tool for user management (`linuxcampam add/train/test`)
- Hardware acceleration support: OpenCL (AMD, Intel), CUDA (NVIDIA)
- Dual-camera support (IR + RGB) with adaptive authentication policies
- Auto-detection of cameras and hardware capabilities
- Smart OpenCL backend detection (uses Rusticl on AMD, prevents Mesa/Clover crashes)
- YuNet face detection and SFace face recognition (ONNX models)
- Debian packaging support (CPack)
- Cross-compilation support for AARCH64 and RISC-V
- Comprehensive security assessment documentation

### Security in 0.9.0

- World-accessible socket (`0666`) for PAM module connectivity during login
- Username sanitization to prevent path traversal
- Face embeddings stored in protected `/etc/linuxcampam/users/`
