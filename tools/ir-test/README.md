# IR camera test page

A local page for checking the IR camera and tuning face login settings. It's separate from the main build and doesn't change `src/`.

## Run it

Live feed and face test, as your normal user:

```bash
python3 tools/ir-test/server.py
```

To add live scores from the real login code, build once and start with `sudo`:

```bash
tools/ir-test/build.sh
sudo python3 tools/ir-test/server.py
```

Under `sudo`, the page doesn't open by itself, so open the URL it prints. The build needs the project's `build/` folder (`make all`). Run `build.sh` again after rebuilding the project, so scores come from the same code as your daemon.

| Option | Default | Purpose |
| :--- | :--- | :--- |
| `--device` | `/dev/video2` | IR camera device |
| `--size` | `640x360` | IR capture size |
| `--port` | `8765` | Localhost port |
| `--no-open` | off | Don't open a browser |
| `--user` | the user who ran `sudo` | Enrolled user to score |
| `--config` | `/etc/linuxcampam/config.ini` | Config used for live scores |

## What each part does

| Feature | Uses | Lockout | Saved images |
| :--- | :--- | :--- | :--- |
| **Live feed** | `ffmpeg` on the IR camera | Not affected | None |
| **Run face test** | `linuxcampam test` through the daemon | Failures count | Failures saved if `save_fail_images` is on |
| **Live scores** | `live_scan`, linked to the real engine | Not affected | None |

The live feed applies the daemon's frame rules from `src/service/camera_frames.cpp`, so it shows when a burst of frames would be rejected. It doesn't detect or match faces.

## Reading live scores

Each round is one complete login scan and takes a few seconds, because it reopens the cameras like a real login does.

- **Score:** similarity to the enrolled face, compared with `threshold`. It isn't a percentage of certainty. Bad mismatches show as `0.000`, because the matching code starts at 0.
- **Boxes:** green or red is the face that was compared, and amber faces were ignored.
- **No face:** a face under `detection_threshold` gets no score, so this doesn't show how close it was.
- **Capture failed:** no usable IR burst. Auto-exposure still changing just after the camera opens is one likely cause.
- **Not reached:** the scan stopped before this camera, for example because a required camera failed.
- **What-if threshold:** only recounts the results on the page. It doesn't change any settings.

To try a setting, edit the config and restart the page. Then run `sudo systemctl restart linuxcampam` so real logins use it too.

## Limits

- The daemon's proximity enforcement isn't applied, because it lives outside the engine.
- `live_scan` doesn't see the daemon's lockout state.

## Safety

- **Close it when you're done.** The feed holds the camera, which makes real face logins fail, and live scans can collide with them.
- **Scores help tune a spoof as well as a login.** With `sudo`, `live_scan` stays root while the page runs, and anything that gets the token can request scans and frames. The server drops to your user, listens on localhost only, and needs the token in the printed URL.
