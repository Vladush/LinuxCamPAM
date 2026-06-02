---
title: LINUXCAMPAMD
section: 8
header: LinuxCamPAM System Manager's Manual
footer: LinuxCamPAM 0.9.7.4
date: June 2026
---

<!-- markdownlint-disable MD025 -->

# NAME

linuxcampamd - LinuxCamPAM background daemon

# SYNOPSIS

**linuxcampamd** [**-d**|**--debug**]

# DESCRIPTION

**linuxcampamd** is the background service responsible for loading AI models, managing camera hardware access, and processing authentication requests from the PAM module.

It listens on a UNIX socket (`/run/linuxcampam/socket` by default) for requests from **pam_linuxcampam**(8) and **linuxcampam**(1).

# OPTIONS

**-d**, **--debug**
:   Enable debug logging to standard output/syslog.

# FILES

*/etc/linuxcampam/config.ini*
:   Configuration file read on startup.

*/usr/share/linuxcampam/models/*
:   Directory containing ONNX AI models (YuNet/SFace).

# LOGGING

Logs are sent to syslog with the tag `linuxcampamd`. Use `journalctl -u linuxcampam` to view logs.

# SEE ALSO

**linuxcampam**(1), **linuxcampam.conf**(5), **pam_linuxcampam**(8)
