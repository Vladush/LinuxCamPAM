# IR Camera — Debug & Troubleshooting Guide

*This guide is based on contributions by [amletoflorio](https://github.com/amletoflorio).*

## Symptom

```text
Response: ENROLL_FAIL Found 0 faces in ir. Expecting exactly 1.
```

## Root Cause

The YuNet face detector assigns **significantly lower confidence scores** to faces captured by IR cameras because they produce grayscale images with low contrast and are often overexposed or underexposed depending on the emitter.

The default `detection_threshold` of `0.9` was tuned for standard RGB webcams. On IR cameras it is nearly impossible to reach this score. Lowering it to `0.5–0.6` resolves the issue in most cases.

---

## Quick Fix

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

## Diagnosing with Logs

During enrollment, watch the logs in real time:

```bash
journalctl -u linuxcampam -f
```

What to look for:

| Log message | Meaning |
| ------------- | --------- |
| `0 faces found above threshold (0.9)` | Threshold too high → lower it |
| `Best score: 0.65 / Threshold: 0.9` | Face found but below threshold → lower your config to 0.60 |
| `Brightness: 12` | Frame nearly black → IR emitter not activating |
| `Brightness: 240` | Frame overexposed → camera exposure issue |

---

## Inspecting the Failed Frame

On every failed enrollment, the captured frame is automatically saved to help you debug:

```text
/var/log/linuxcampam/failed_enroll_<camera_id>_<username>.jpg
```

Open it to check what the camera actually captured:

```bash
xdg-open /var/log/linuxcampam/failed_enroll_cam_ir_username.jpg
```

- **Nearly black frame** → IR emitter did not activate (see section below)
- **Grainy / blurry frame** → increase `enroll_average_frames`
- **Face visible but not detected** → lower `detection_threshold` further
- **No face in frame** → positioning issue during enrollment

---

## IR Emitter Not Activating

If the saved frame is nearly black, your IR lights are not turning on.

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

   Follow the interactive procedure — move your head in front of the camera while it tries different configurations. When the emitter blinks and you see `The infrared emitter has been successfully enabled!` you are done.

---

## Finding the Correct IR Camera Device

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

The IR camera has **1 channel** (shape like `(360, 640)`) instead of 3 (RGB). On many laptops (e.g., Lenovo, Dell) it is typically `/dev/video2`.
