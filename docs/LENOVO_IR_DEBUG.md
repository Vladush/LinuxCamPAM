# Lenovo IR Camera — Debug Guide

## Symptom

```
Response: ENROLL_FAIL Found 0 faces in ir. Expecting exactly 1.
```

## Root Cause

The YuNet face detector assigns **significantly lower confidence scores** to faces
captured by IR cameras (grayscale image, low contrast, often overexposed).

The default threshold of `0.9` was tuned for standard RGB webcams. On IR cameras
it is nearly impossible to reach. Lowering it to `0.5–0.6` resolves the issue
in most cases.

---

## Quick Fix (no recompilation required)

Edit `/etc/linuxcampam/config.ini`:

```ini
[Auth]
detection_threshold = 0.5

[Capture]
enroll_averaging = on
enroll_average_frames = 7
```

Then restart the daemon:
```bash
sudo systemctl restart linuxcampam
```

---

## Diagnosing with logs

During enrollment, watch the logs in real time:

```bash
journalctl -u linuxcampam -f
```

What to look for:

| Log message | Meaning |
|-------------|---------|
| `0 faces found above threshold (0.9)` | Threshold too high → lower it |
| `Best score: 0.65 \| Threshold: 0.9` | Face found but below threshold → lower to 0.55 |
| `Brightness: 12` | Frame nearly black → IR emitter not activating |
| `Brightness: 240` | Frame overexposed → camera exposure issue |

---

## Inspecting the failed frame

On every failed enrollment the captured frame is automatically saved to:

```
/var/log/linuxcampam/failed_enroll_ir_<username>.jpg
```

Open it to check what the camera actually captured:
```bash
xdg-open /var/log/linuxcampam/failed_enroll_ir_<username>.jpg
```

- **Nearly black frame** → IR emitter did not activate (see section below)
- **Grainy / blurry frame** → increase `enroll_average_frames`
- **Face visible but not detected** → lower `detection_threshold` further
- **No face in frame** → positioning issue during enrollment

---

## IR emitter not activating

If the saved frame is nearly black:

1. Verify `linux-enable-ir-emitter` is installed:
   ```bash
   ls /usr/local/bin/linux-enable-ir-emitter
   ```

2. Test it manually:
   ```bash
   sudo linux-enable-ir-emitter run
   ```

3. If not installed:
   ```bash
   sudo apt install linux-enable-ir-emitter
   # or from source: https://github.com/EmixamPP/linux-enable-ir-emitter
   ```

4. Configure for your specific hardware model:
   ```bash
   sudo linux-enable-ir-emitter configure
   ```
   Follow the interactive procedure — move your head in front of the camera
   while it tries different configurations. When the emitter blinks and you see
   `The infrared emitter has been successfully enabled!` you are done.

---

## Finding the correct IR camera device

```bash
# List all webcams
v4l2-ctl --list-devices

# Identify the IR camera by checking shape and brightness
python3 -c "
import cv2
for i in range(4):
    cap = cv2.VideoCapture(i)
    if not cap.isOpened():
        print(f'video{i}: could not open')
        continue
    ret, frame = cap.read()
    cap.release()
    if ret:
        print(f'video{i}: shape={frame.shape} brightness={frame.mean():.0f}')
"
```

The IR camera has **1 channel** (shape like `(360, 640)`) instead of 3 (RGB).
On Lenovo laptops it is typically `/dev/video2`.

---

## Recommended settings for Lenovo ThinkPad / IdeaPad

```ini
[Auth]
detection_threshold = 0.5
timeout_ms = 5000

[Capture]
enroll_hdr = off               ; IR cameras do not support HDR
enroll_averaging = on
enroll_average_frames = 7

[Hardware]
camera_path_ir = /dev/video2   ; verify with v4l2-ctl --list-devices
```

---

## Code changes (this fix)

| File | Change |
|------|--------|
| `include/constants.hpp` | `IR_TRIGGER_DELAY_MS`: 200 → 1500 ms |
| `include/constants.hpp` | `CAMERA_WARMUP_FRAMES`: 10 → 15 |
| `include/constants.hpp` | `CAMERA_WARMUP_DELAY_MS`: 100 → 200 ms |
| `src/service/config.hpp` | `DEFAULT_DETECTION_THRESHOLD`: 0.9 → 0.6 |
| `src/service/auth_engine.cpp` | Log YuNet score + brightness in `generateEmbedding` |
| `src/service/auth_engine.cpp` | Always save failed enrollment frame with diagnostic info |
| `config/config.ini` | Updated default `detection_threshold` + averaging enabled |
