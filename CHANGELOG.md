# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.9.3] - 2026-01-03

### Fixed

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

### Improved

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
