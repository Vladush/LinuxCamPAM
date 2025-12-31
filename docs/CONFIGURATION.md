# LinuxCamPAM Configuration Guide

## Multi-Camera Support

LinuxCamPAM now supports an arbitrary number of cameras with configurable authentication policies. This allows for dual-camera setups (IR + RGB), single-camera setups, or custom multi-view configurations.

### Configuration (`/etc/linuxcampam/config.ini`)

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

Control how multiple cameras determine the final result in the `[Auth]` section.

```ini
[Auth]
policy = adaptive
```

**Available Policies:**

- **adaptive** (Default):
  - Designed for IR+RGB setups.
  - "IR" cameras (type=`ir`) are **Critical**: Failure to capture or match will fail authentication.
  - "RGB" or other cameras are **Conditional**: They must match *only if* they are participating (capture succeeded and brightness > min_brightness).
  - Matches the legacy behavior of LinuxCamPAM.
- **strict**:
  - **All** cameras defined in `[Cameras]` must successfully capture, pass brightness check, AND match the user.
  - If any camera fails (even if dark), authentication fails.
- **lenient**:
  - **At least one** camera must successfully capture, pass brightness check, and match the user.
  - Typically used for "Either Camera A OR Camera B" scenarios.

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
