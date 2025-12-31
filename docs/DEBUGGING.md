# Debugging LinuxCamPAM

This guide helps you troubleshoot issues with the LinuxCamPAM service. There are two ways to view logs:

1. **Systemd Journal** (Recommended for most distros).
2. **Direct Log File** (For non-systemd systems like Gentoo).

## 1. Checking Logs

LinuxCamPAM runs as a systemd service (`linuxcampamd`). Logs are managed by `journalctl`.

### View Live Logs

To follow the logs in real-time while you authenticate or run commands:

```bash
sudo journalctl -u linuxcampam -f
```

### View Recent History

To see the last 50 log entries:

```bash
sudo journalctl -u linuxcampam -n 50 --no-pager
```

## 2. Enabling Verbose Logging

By default, the service runs at `INFO` level, which is quiet. To see details like face matching scores, brightness levels, and camera backend initialization:

1. Edit the configuration file:

    ```bash
    sudo nano /etc/linuxcampam/config.ini
    ```

2. Add or update the `[General]` section:

    ```ini
    [General]
    log_level = debug
    ```

3. Restart the service:

    ```bash
    sudo systemctl restart linuxcampam
    ```

**Log Levels:**

* `error`: critical failures only.
* `warn`: non-critical issues (e.g., camera frame dropped, low brightness).
* `info`: (Default) startup, shutdown, authentication success/fail events.
* `debug`: verbose details (scores, timings, backend info).

## 3. Log Files (Non-Systemd)

If you are using a distribution without systemd (e.g., Gentoo, Alpine, Void), standard output might not be captured automatically. You can configure a direct log file:

```ini
[General]
log_file = /var/log/linuxcampam.log
```

Ensure the service user (usually `root` for now) has write permissions to this file.

## 4. Building for Debugging

If you need gdb symbols or want to disable compiler optimizations to step through code:

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make
```

Then run the service manually from the build directory (requires `sudo` for access to `/etc/linuxcampam`):

```bash
sudo ./linuxcampamd
```

## 5. Common Issues

### "Camera failed to capture"

* **Cause:** Another application (Zoom, browser) is using the camera.
* **Fix:** Close other apps. LinuxCamPAM retries 3 times before failing.

### "Enroll failed: Found 2 faces"

* **Cause:** Multiple people are visible in the camera frame.
* **Fix:** Ensure only the user being enrolled is visible.

### "No AI providers available"

* **Cause:** OpenCV could not load the ONNX models.
* **Fix:** Check if model files exist in `/etc/linuxcampam/models/`.

### "Service not listening" / PAM errors

* **Cause:** The service crashed or socket permissions are wrong.
* **Fix:** Check `sudo systemctl status linuxcampam`. Ensure `/run/linuxcampam/users/` exists.
