---
title: LINUXCAMPAM.CONF
section: 5
header: LinuxCamPAM Configuration File
footer: LinuxCamPAM 0.9.7.4
date: June 2026
---

<!-- markdownlint-disable MD025 -->

# NAME

linuxcampam.conf - Configuration file for LinuxCamPAM

# SYNOPSIS

/etc/linuxcampam/config.ini

# DESCRIPTION

The **linuxcampam.conf** file (located at `/etc/linuxcampam/config.ini`) controls the behavior of the **linuxcampamd**(8) service and the **pam_linuxcampam**(8) module.

The file is in standard INI format. Sections and keys are case-insensitive.

# SECTIONS

## [General]

**log_level** = *LEVEL*
:   Logging verbosity.
    Values: `debug`, `info`, `warning`, `error` (Default: `info`).

**log_file** = *PATH*
:   Path to log file when not running under systemd.
    (Default: `/var/log/linuxcampam.log`).

## [Auth]

**threshold** = *FLOAT*
:   Similarity threshold for face matching (0.0 - 1.0).
    Lower is stricter. (Default: `0.363`).

**detection_threshold** = *FLOAT*
:   Confidence threshold for face detection (0.0 - 1.0).
    (Default: `0.9`).

**timeout_ms** = *INT*
:   Timeout in milliseconds to wait for a successful match.
    (Default: `3000`).

**policy** = *MODE*
:   Authentication policy.
    Values: `adaptive` (smart logic), `strict` (all cameras match), `lenient` (any camera match).
    (Default: `adaptive`).

## [Security]

**lockout_attempts** = *INT*
:   Number of consecutive failed attempts before lockout.
    (Default: `5`).

**lockout_duration_sec** = *INT*
:   Duration of lockout in seconds.
    (Default: `300`).

**min_uid** = *INT*
:   Minimum user ID (UID) required to attempt face authentication. System users (e.g., root, daemon) with UIDs lower than this are ignored and fall back to password automatically.
    (Default: `1000`).

**show_welcome** = *BOOL*
:   Whether to show a welcome message upon successful authentication.
    (Default: `true`).

**welcome_message** = *STRING*
:   Custom welcome message to display if `show_welcome` is enabled. Use `%u` to inject the username into the message. Quotes are supported but optional.
    (Default: `LinuxCamPAM: Welcome, %u!`).

## [Hardware]

**provider_priority** = *LIST*
:   Comma-separated list of hardware backends to try.
    (Default: `OpenCL,CPU`).

**camera_path_ir** = *PATH*
:   Path to IR camera (e.g., `/dev/video2`).

**camera_path_rgb** = *PATH*
:   Path to RGB camera (e.g., `/dev/video0`).

## [Performance]

**model_keep_alive_sec** = *INT*
:   Seconds to keep AI models in memory.
    (Default: `0`).

**gpu_flush** = *BOOL*
:   Explicit sync after GPU inference (fixes AMD driver hangs).
    (Default: `on`).

**gpu_throttle_ms** = *INT*
:   Sleep time between heavy GPU operations.
    (Default: `20`).

## [Capture]

**enroll_hdr** = *MODE*
:   Uses multiple exposures if camera supports manual exposure control.
    Values: `auto`, `on`, `off`. (Default: `auto`).

**enroll_averaging** = *BOOL*
:   Reduce noise by averaging frames during enrollment.
    (Default: `on`).

**enroll_average_frames** = *INT*
:   Number of frames to average during enrollment.
    (Default: `5`).

**verify_averaging** = *BOOL*
:   Use frame averaging during authentication (slower, but more reliable).
    (Default: `off`).

**verify_average_frames** = *INT*
:   Number of frames to average during verification if averaging is enabled.
    (Default: `3`).

## [Storage]

**save_fail_images** = *BOOL*
:   Save images of failed attempts for debugging.
    (Default: `false`).

**save_success_images** = *BOOL*
:   Save images of successful authentications.
    (Default: `false`).

# COMMANDS

**Advanced Camera Configuration**
:   Use `[Camera.NAME]` sections to define multiple cameras with specific roles (ir/rgb) and options.
    See `config.ini` for details.

# SEE ALSO

**linuxcampam**(1), **linuxcampamd**(8), **pam_linuxcampam**(8)
