# LinuxCamPAM Configuration Guide

## Multi-Camera Support

LinuxCamPAM now supports an arbitrary number of cameras with configurable authentication policies. This allows for dual-camera setups (IR + RGB), single-camera setups, or custom multi-view configurations.

### Configuration (`/etc/linuxcampam/config.ini`)

#### General Settings

Control logging verbosity:

```ini
[General]
log_level = info  ; options: debug, info, warning, error
```

#### 1. Define Cameras

List the identifiers for your cameras in the `[Cameras]` section.

```ini
[Cameras]
names = cam_ir, cam_rgb
```

#### 2. Configure Each Camera

Create a section `[Camera.<id>]` for each identifier defined above.

```ini
[Camera.cam_ir]
path = /dev/video2
type = ir
min_brightness = 0  ; 0 = no brightness check (always use if available)

[Camera.cam_rgb]
path = /dev/video0
type = rgb
min_brightness = 40 ; Only participate if scene brightness > 40
```

- **path**: Device path (e.g., `/dev/video0`).
- **type**: Camera type tag (`ir`, `rgb`, or custom). Used for matching user profile data (`embedding_<type>`).
- **min_brightness**: Minimum average pixel intensity (0-255). If a camera's image is darker than this, it is skipped (unless `mandatory=true`).
- **mandatory**: `true` or `false` (default: `false`). Only used in **Adaptive** policy.
  - If `true`, this camera matches are **required**. Failure to capture or match (or being too dark) will cause authentication failure.
  - If `false`, this camera is conditional. It contributes if valid, but its failure (or darkness) does not fail auth immediately (unless no cameras participate).

#### 3. Authentication Policy

Control how cameras behave and set recognition thresholds in the `[Auth]` section.

```ini
[Auth]
policy = adaptive
threshold = 0.363           ; Similarity threshold (lower = stricter). Default ~0.36-0.4.
detection_threshold = 0.9   ; Face detection confidence (0.0-1.0).
timeout_ms = 3000           ; Auth timeout in milliseconds.
max_embeddings = 5          ; Max face profiles per user (0 = unlimited).
```

**Available Policies:**

- **adaptive** (Default):
  - Designed for IR+RGB setups.
  - "IR" cameras (type=`ir`) are **Critical**: Failure to capture or match will fail authentication.
  - "RGB" or other cameras are **Conditional**: They must match *only if* they are participating (capture succeeded and brightness > min_brightness).
- **strict**:
  - **All** cameras defined in `[Cameras]` must successfully capture, pass brightness check, AND match the user.
  - If any camera fails (even if dark), authentication fails.
- **lenient**:
  - **At least one** camera must successfully capture, pass brightness check, and match the user.
  - Typically used for "Either Camera A OR Camera B" scenarios.

#### 4. Capture Settings (Advanced)

Control image quality logic to improve enrollment and verification success rates in the `[Capture]` section. These can also be overridden per-camera.

```ini
[Capture]
enroll_hdr = auto             ; auto | on | off. Uses multi-exposure if supported.
enroll_averaging = on         ; true | false. Reduce noise by averaging frames.
enroll_average_frames = 5     ; Number of frames to average.

verify_averaging = false      ; Averaging during auth (slower, but more reliable).
verify_average_frames = 3
```

- **enroll_hdr**: Improves profiles in difficult lighting (backlit scenes).
- **enroll_averaging**: recommended for IR cameras to reduce sensor noise.

### Smart Defaults & Backward Compatibility

### Smart Setup Tool

A helper script `linuxcampam-setup-config` is provided (and run at install time) to automatically detect connected cameras using `v4l2-ctl` and generate a compatible configuration. It is recommended to run this tool first before manually editing the config.

If the `[Cameras]` section is missing, the system attempts to auto-detect your configuration:

1. **Explicit Legacy Config**: If `Hardware.camera_path_ir` or `Hardware.camera_path_rgb` are present in `config.ini` (old format), they are honored.
2. **Auto-Detection**: If no configuration is present:
   - **Dual Setup**: If both `/dev/video2` and `/dev/video0` exist:
     - Configures IR (`/dev/video2`) as **Mandatory**.
     - Configures RGB (`/dev/video0`) as **Conditional** (min_brightness=40).
   - **Single RGB**: If only `/dev/video0` exists:
     - Configures it as **Mandatory**.
   - **Single IR**: If only `/dev/video2` exists:
     - Configures it as **Mandatory**.
   - **Fallback**: Defaults to `/dev/video0` as generic mandatory if specific paths aren't found.

### Security Settings

Protect against brute-force attacks by limiting the number of consecutive failures.

```ini
[Security]
lockout_attempts = 5
lockout_duration_sec = 300
```

- **lockout_attempts**: Number of failed attempts before temporary lockout.
- **lockout_duration_sec**: Duration of lockout in seconds (default 300s = 5 minutes).

### GPU Stability

If you experience system freezes with OpenCL (common with Mesa Rusticl on AMD), enable explicit synchronization:

```ini
[Performance]
gpu_flush = on
gpu_throttle_ms = 50
```

- **gpu_flush**: Force GPU sync after inference. Default: `on` (safe mode).
- **gpu_throttle_ms**: Sleep time between heavy GPU operations. Default: `20` (prevents UI lag on integrated graphics).

**Note:** The daemon wipes `~/.cache/opencv` on startup - old cached kernels can cause hangs after Mesa updates.
