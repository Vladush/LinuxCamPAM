---
title: LINUXCAMPAM
section: 1
header: LinuxCamPAM User Manual
footer: LinuxCamPAM 0.9.7.2
date: February 2026
---

<!-- markdownlint-disable MD025 -->

# NAME

linuxcampam - Command line interface for LinuxCamPAM face authentication system

# SYNOPSIS

**linuxcampam** *COMMAND* [*ARGS*...]

# DESCRIPTION

**linuxcampam** is the administration and testing utility for the LinuxCamPAM face authentication system. It communicates with the background daemon (**linuxcampamd**) to enroll users, manage face embeddings, and test authentication configuration.

# COMMANDS

**add** *USERNAME*
:   Enroll a new user. This limits the face capture to ensure high quality face data is stored.
    **Requires root privileges.**

**train** [*USERNAME*] [*OPTIONS*]
:   Train or refine the model for a user.
    **Requires root privileges** (unless training self and permissions allow).

    **--label** *NAME*
    :   Refine a specific embedding label (e.g., "glasses", "dark").
    
    **--new**
    :   Add a new embedding variant for the user.

**test** [*USERNAME*]
:   Test camera capture and authentication. If *USERNAME* is provided, verifies against that user's embeddings. If omitted, tests basic camera functionality.

    *Note:* Testing another user **requires root privileges**.

**list** *USERNAME*
:   List all stored embedding labels for a user.
    **Requires root privileges** (unless listing self).

**remove** *USERNAME* **--label** *NAME*
:   Remove a specific embedding variant for a user.
    **Requires root privileges.**

**show-config**
:   Display the currently active configuration merged from defaults and config file.

**debug** [*on*|*off*]
:   Toggle debug logging in the daemon at runtime.

**version**
:   Show client and daemon version information.

**help**
:   Show help message.

# EXIT STATUS

Returns **0** on success, non-zero on error.

# FILES

*/etc/linuxcampam/config.ini*
:   Main configuration file.

*/run/linuxcampam/socket*
:   UNIX socket for daemon communication.

# SEE ALSO

**linuxcampamd**(8), **linuxcampam.conf**(5), **pam_linuxcampam**(8)
