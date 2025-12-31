#pragma once

// Shared constants for LinuxCamPAM
namespace linuxcampam {
constexpr const char *SOCKET_PATH = "/run/linuxcampam/socket";
constexpr const char *CONFIG_PATH = "/etc/linuxcampam/config.ini";
constexpr const char *USERS_DIR = "/etc/linuxcampam/users";
constexpr const char *MODELS_DIR = "/etc/linuxcampam/models";
constexpr const char *IR_EMITTER_PATH =
    "/usr/local/bin/linux-enable-ir-emitter";
} // namespace linuxcampam
