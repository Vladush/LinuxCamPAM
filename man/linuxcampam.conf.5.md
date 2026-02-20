---
title: LINUXCAMPAM.CONF
section: 5
header: LinuxCamPAM Configuration File
footer: LinuxCamPAM 0.9.7.2
date: February 2026
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
    Lower is stricter. (Default: `0.4`).

**detection_threshold** = *FLOAT*
:   Confidence threshold for face detection (0.0 - 1.0).
    (Default: `0.6`).

**timeout_ms** = *INT*
:   Timeout in milliseconds to wait for a successful match.
    (Default: `3000`).

**policy** = *MODE*
:   Authentication policy.
    Values: `adaptive` (smart logic), `strict` (all cameras match), `lenient` (any camera match).
    (Default: `adaptive`).

## [Hardware]

**provider_priority** = *LIST*
:   Comma-separated list of hardware backends to try.
    (Default: `OpenCL,OpenVINO,CUDA,CPU`).

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
    (Default: `off`).

## [Storage]

**save_fail_images** = *BOOL*
:   Save images of failed attempts for debugging.
    (Default: `true`).

**save_success_images** = *BOOL*
:   Save images of successful authentications.
    (Default: `false`).

# COMMANDS

**Advanced Camera Configuration**
:   Use `[Camera.NAME]` sections to define multiple cameras with specific roles (ir/rgb) and options.
    See `config.ini` for details.

# SEE ALSO

**linuxcampam**(1), **linuxcampamd**(8), **pam_linuxcampam**(8)
