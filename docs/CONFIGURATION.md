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

> **Tip:** Not sure which policy to use? See the [Configuration Decision Tree](USER_FLOWS.md#2-configuration-helper-which-setup-is-right-for-me).

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
enroll_averaging = on         ; on | off. Reduce noise by averaging frames.
enroll_average_frames = 5     ; Number of frames to average.

verify_averaging = off        ; Averaging during auth (slower, but more reliable).
verify_average_frames = 3
```

- **enroll_hdr**: Improves profiles in difficult lighting (backlit scenes).
- **enroll_averaging**: recommended for IR cameras to reduce sensor noise.

#### 5. Proximity Sensor (Optional)

Configure native support for human presence sensors (like the ITE8353) to detect human presence and optimize authentication.

```ini
[Hardware]
proximity_sensor = auto
proximity_sensor_id = ITE8353
proximity_enforce = false
```

- **proximity_sensor**: `auto` (use if found), `enabled` (fail if not found), or `disabled`.
- **proximity_sensor_id**: The ACPI/I2C identifier for the sensor (default: `ITE8353`).
- **proximity_enforce**: If `true`, authentication will instantly fail if the proximity sensor reports no human is present. If `false` (default), the sensor provides observational presence data without blocking authentication.

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

### System Integration (UID Filtering)

By default, the module ignores system users (like `sddm` or `gdm`) to prevent the camera from turning on during the login screen initialization. This prevents conflicts where the greeter grabs the camera before you can.

You can configure this behavior in the `[Security]` section:

```ini
[Security]
min_uid = 1000
show_welcome = true
welcome_message = "LinuxCamPAM: Welcome, %u!"
```

- **Standard Method (Recommended)**: Leave `min_uid` at `1000`. This effectively tells the module: "If the user has a UID less than 1000, don't even try to authenticate them." This is the safest and easiest way to avoid boot-up loops.
- **Manual PAM Method (Advanced)**: Set `min_uid = 0` to disable this check. Do this only if you want to control everything yourself using PAM configuration files (e.g., adding `pam_succeed_if.so uid >= 1000` manually in `/etc/pam.d/common-auth`). If you disable this setting and don't add your own safeguards, your login screen might hang while it waits for a face that isn't there.

### Welcome Message

By default, a welcome message is displayed upon successful authentication. You can customize or disable it in the `[Security]` section:

- **show_welcome**: `true` or `false`. If `false`, the module succeeds silently. This can also be overridden per-auth by passing `no_welcome` as a PAM argument in `/etc/pam.d/`.
- **welcome_message**: Custom string. Use `%u` to inject the username. Quotes are optional but recommended for strings with spaces.
- **Compile-time Override**: If the module was compiled with `cmake -DDISABLE_WELCOME_MESSAGE=ON ..`, all welcome message logic is stripped for maximum security/stealth. INI settings and PAM arguments will have no effect.

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
