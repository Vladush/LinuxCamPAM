# 3. Use UNIX Domain Sockets for IPC

Date: 2026-06-02

## Status

Accepted

## Context

LinuxCamPAM requires a split architecture:
1.  A PAM module (`pam_linuxcampam.so`) that runs in the context of the calling application (e.g., `sudo`, `gdm`).
2.  A background daemon (`linuxcampamd`) that runs as root to access `/dev/video*` devices and load heavy AI models.

We needed an Inter-Process Communication (IPC) mechanism to allow the PAM module to ask the daemon for authentication.

Alternative solutions considered:
1.  **D-Bus**: The standard Linux IPC mechanism. However, integrating D-Bus inside a PAM module is notoriously difficult, especially under Wayland or during early boot phases, leading to deadlocks.
2.  **TCP/UDP Sockets**: Unnecessary overhead and exposes the service to the network stack.

## Decision

We chose to use standard **UNIX Domain Sockets** (`/run/linuxcampam/socket`) with `0666` (World Read/Write) permissions.

## Consequences

*   **Positive:** Extremely fast and robust. Works perfectly in early-boot (GDM) and headless environments (SSH).
*   **Positive:** Zero dependencies on external message buses (like `dbus-daemon`).
*   **Negative:** The `0666` permission means any local user can ping the socket. 
*   **Mitigation:** The daemon strictly sanitizes all inputs, does not return sensitive data (only `AUTH_SUCCESS` or `AUTH_FAIL`), and rate-limits requests to prevent local DoS attacks. Real face data remains securely shielded behind the daemon.
