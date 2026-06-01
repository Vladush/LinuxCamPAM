# 2. Use OpenCL for Hardware Acceleration

Date: 2026-06-02

## Status

Accepted

## Context

Face detection (YuNet) and face recognition (SFace) are computationally heavy processes that perform matrix operations. CPU-only inference can cause a noticeable delay (1-2 seconds) during PAM authentication, which negatively impacts the user experience (e.g., when typing `sudo`). 

We needed a way to accelerate these ONNX models using the host machine's GPU or NPU.

Alternative solutions considered:
1.  **Native CUDA / cuDNN**: Requires proprietary NVIDIA drivers, headers, and massive library dependencies (~500MB+). Only supports NVIDIA.
2.  **Native OpenVINO**: Extremely performant on Intel hardware, but requires bundling massive static libraries (~300MB+) and doesn't natively support AMD/NVIDIA without complex plugins.
3.  **OpenCV Transparent API (T-API) via OpenCL**: OpenCV provides an abstraction layer that can route OpenVX/OpenCL instructions dynamically.

## Decision

We chose to use **OpenCV T-API via OpenCL** as our primary hardware acceleration backend. 

By prioritizing OpenCL, we rely on the OS's existing OpenCL ICD loaders (e.g., `intel-opencl-icd`, `mesa-opencl-icd`, `nvidia-opencl-icd`).

## Consequences

*   **Positive:** The compiled binary remains extremely small (~15MB statically linked).
*   **Positive:** A single binary works out-of-the-box on Intel, AMD, NVIDIA, and ARM GPUs as long as an OpenCL runtime is present.
*   **Negative:** OpenCL overhead means it's slightly slower than bare-metal CUDA or OpenVINO, but the difference in a single-frame inference scenario is negligible (<50ms).
*   **Negative:** Certain open-source AMD drivers (like Rusticl) occasionally hang during OpenCL memory flushes, requiring us to implement a `gpu_flush` workaround in our config.
