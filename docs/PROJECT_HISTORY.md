# Project History & Decision Log

This document tracks the evolution of LinuxCamPAM, focusing on the technical decisions, architectural choices, and the reasoning behind them.

> **Note:** This project is, at its core, a private hobby undertaking. It was built primarily to satisfy my personal need for seamless authentication on my specific hardware. Making this code "publicly ripe" took significant time and effort during the Christmas/New Year holidays, inspired by a recent experience publishing another task on GitHub. While I have attempted to create it with high engineering standards, the decisions below reflect my personal preferences, hardware constraints, and curiosity.

## 0. Origin & Motivation

**The Catalyst:**
The project began with a specific goal: utilizing the NPU on a **Lenovo Yoga 7 Pro (2025, 14ASP9)** for face authentication to save power and keep the main GPU available for other workloads. It was also driven by pure curiosity and the fun of engineering a custom solution.

**The "Howdy" Experience:**
The initial attempt involved forking the popular **Howdy** project to enable NPU support (XDNA/XRT). However, this path proved fraught with challenges:

* **Complexity:** Tweaking DKMS and kernel drivers for XDNA support eventually destabilized the system. While recoverable, I opted for a full OS re-installation as the fastest path to restore a working environment needed for urgent tasks. (Lesson learned: don't experiment on your daily driver!)
* **Performance:** Post-reinstall, Howdy's recognition was unreliable and slow. It blocked PAM for several seconds—making it faster to simply type a password.
* **Dependency Hell:** Managing global Python modules and compiling `dlib` on Kubuntu 24.04 was brittle and "messy" from a system administration perspective.

**The Solution:**
These frustrations led to the decision to architect a new solution from scratch: **LinuxCamPAM**.

* **Goal:** A native C++ application that is instant, stable, and system-native.
* **Approach:** Eliminate the Python startup cost and "dependency hell" by using a compiled service and standard libraries.

## 1. Architectural Foundation

### Decision: Client-Server Architecture (Service + PAM Module)

**Context:** Many existing solutions (like Howdy) execute Python scripts directly from PAM.
**Problem:** I found this incurs a high "startup tax" (interpreter load + model load) for every authentication attempt. The 2-3 second delay before face scanning even began was personally annoying to me.
**Decision:** The project was split into two components:

1. **Service (C++)**: Runs in the background, keeping AI models loaded in RAM.
2. **PAM Module / CLI**: Lightweight clients that send a specific signal to the service via Unix Socket.
**Outcome:** Authentication is "instant" (sub-100ms) because the heavy lifting is already done before the user even presses Enter.

### Decision: C++ over Python

**Context:** Python is the standard for AI/ML prototyping.
**Reasoning:**

* **Performance:** Critical for system-level authentication.
* **Dependencies:** Python environments on Linux are fragile (system updates often break pip packages). A compiled C++ binary with standard system libraries (`libopencv`, `libpam`) is far more stable and easier to package.

## 2. AI & Models

### Decision: OpenCV DNN + ONNX (YuNet & SFace)

**Context:** A face recognition engine was required. Options included Dlib (classic, heavy), TensorFlow/PyTorch (massive dependencies), or proprietary SDKs.
**Decision:** `OpenCV::DNN` was selected, specifically running:

* **YuNet**: For lightweight, millisecond-scale face detection.
* **SFace**: For robust recognition with small embedding size.
**Reasoning:** These models are "State of the Art" for lightweight edge computing. They allow the system to run on anything from a high-end workstation to a Raspberry Pi Zero, without requiring a 2GB PyTorch installation.

### Decision: Agnostic Hardware Acceleration

**Context:** Users have NVIDIA (CUDA), AMD (ROCm), Intel (OpenVINO), or just CPUs.
**Decision:** Instead of writing vendor-specific code, the system leverages OpenCV's transparent API (`cv::ocl`) and DNN backends.
**Outcome:** The system automatically selects the best available provider in the order: ROCm > OpenVINO > CUDA > CPU.

## 3. Installation & User Experience

### Decision: Smart Camera Discovery

**Context:** Configuring camera paths (`/dev/videoX`) manually is tedious and error-prone. I didn't want to dig through `v4l2-ctl` output every time I set this up.
**Decision:** Implementation of `setup_config.sh` which uses `v4l2-ctl` to inspect device capabilities.
**Logic:**

* If a camera supports GREY/Y8 formats -> It's likely an IR camera (Preferred).
* If a camera supports MJPG/YUYV -> It's a standard Webcam.
* If both exist -> Configure "Dual Camera Mode" automatically.

### Decision: Safety-First PAM Configuration

**Context:** Editing `/etc/pam.d/common-auth` is dangerous; a syntax error locks the user out of their system.
**Decision:**

1. **Profiles:** A PAM Profile (`/usr/share/pam-configs/linuxcampam`) is installed, and the system tool `pam-auth-update` handles the file editing.
2. **Backups:** The install scripts automatically create timestamped backups of all PAM config files before touching the system configuration.

### Decision: Zero-Interaction Installer

**Context:** Many installers ask "Are you sure? [Y/n]" or require manual file editing.
**Decision:** The installer (`install.sh`) runs start-to-finish without user prompts.
**Reasoning:**

* **Automation:** Allows deployment via configuration management tools (Ansible/Puppet) without `expect` scripts or hanging processes.
* **Determinism:** Eliminates the risk of a user selecting the "wrong" option during a setup wizard. The script detects the environment (Systemd vs Non-Systemd) and applies the correct configuration automatically.

## 4. Optimization & Tuning

### Decision: Hybrid Memory Mode

**Context:** The "Background Service" architecture improved speed but introduced a permanent RAM cost (~60MB). This is negligible on a 32GB workstation but significant on a 2GB Raspberry Pi.
**Decision:** Implemented a dynamic `[Performance]` logic.

* **Mechanism:** If configured, the service "unloads" the AI models from RAM after N seconds of inactivity.
* **Trade-off:** Saves RAM when idle, but re-introduces the "startup delay" for the first authentication after a break.

### Decision: CPU-Aware Defaults (The "Atom" Edge Case)

**Context:** Enabling Hybrid Mode by default for all low-RAM devices was considered.
**Problem:** On a low-RAM device with a *slow* CPU, reloading the models takes 3-4 seconds. I discovered this on my daughter's **Fujitsu LifeBook T900** (1st Gen Core i7, 8GB RAM), where the wake-up delay became a usability bottleneck despite having sufficient RAM.
**Refinement:** The setup script now checks CPU capabilities (specifically `AVX` on x86 or `NEON`/`ASIMD` on ARM).

* **Fast CPU + Low RAM:** Hybrid Mode (Save RAM).
* **Slow CPU + Low RAM:** Instant Mode (Burn RAM to save Speed).

## 5. Multi-Architecture Support

### Decision: Dynamic Library Paths

**Context:** Early build scripts hardcoded `/lib/x86_64-linux-gnu`. This broke support for my 3D printer fleet—specifically my **Voron 0 and Voron 2.4**, which run on **Raspberry Pi 2/3+** nodes.
**Decision:**

* **CMake:** Adopted `GNUInstallDirs` for standard variable handling.
* **Scripts:** Adopted `dpkg-architecture` to detect the correct multi-arch library path at runtime.
**Outcome:** LinuxCamPAM actively supports **amd64**, **i386**, **arm64** (Raspberry Pi/Jetson), and **riscv64**.

## 6. Development Standards

### Decision: C++17 Standard

**Context:** C++20 and C++23 offer powerful features (`std::format`, `std::jthread`, `std::expected`) that would simplify the codebase.
**Decision:** The project strictly enforces **C++17**.
**Reasoning:**

* **Compatibility is King:** A PAM module must be installable on stable/LTS enterprise systems.
* **Target Hardware:** Many target devices (e.g., Jetson Nano running Ubuntu 18.04/20.04, Debian 10 servers) rely on older GCC versions (GCC 8/9).
* **Verdict:** C++17 strikes the balance of providing modern safety (`std::filesystem`, `std::optional`) without alienating 50% of the user base.

### Decision: Standardized Daemon Naming

**Context:** The service binary was originally named `linuxcampam_service`.
**Decision:** Renamed to `linuxcampamd`.
**Reasoning:** Aligns with standard Unix convention where background daemons carry a `d` suffix (`systemd`, `sshd`, `fprintd`).

## 7. Roadmap / Future Work

### Goal: Native NPU Support (Ryzen AI / XDNA)

**Context:** Many modern laptops (like my current Lenovo Yoga 7 Pro 14ASP9) feature powerful NPUs.
**Current State:** LinuxCamPAM currently relies on the CPU or standard GPU (OpenCL/CUDA) for inference.
**Future Plan:**

* Monitor OpenCV's integration of the **ONNX Runtime** backend.
* Once available, enable the specific backend flags to offload inference to AMD Ryzen AI (XDNA) and other NPU architectures.
* **Why wait?** Waiting for upstream OpenCV support ensures the project avoids the "Dependency Hell" of maintaining custom kernel drivers or forked libraries.

### Decision: Deferred Modular Backend (YAGNI)

**Context:** The architecture initially considered abstracting the inference engine into a plugin system or `IBackend` interface to allow mixing OpenCV, ONNX Runtime, or other providers.
**Decision:** This complexity is rejected under the **YAGNI (You Aren't Gonna Need It)** principle.
**Reasoning:**

* **Current Reality:** OpenCV is the single, robust backend currently in use. Creating an abstract interface now would add virtual table overhead and boilerplate for zero immediate benefit.
* **Trigger for Change:** An `IBackend` architecture will be introduced *only if and when* a second implementation is required (e.g., direct `libonnxruntime` for Ryzen AI). This ensures the interface is designed around actual, concrete requirements rather than speculation.
