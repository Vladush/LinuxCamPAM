#pragma once

#include <cstddef>

// Shared constants for LinuxCamPAM
namespace linuxcampam {
inline constexpr const char *SOCKET_PATH = "/run/linuxcampam/socket";
inline constexpr const char *CONFIG_PATH = "/etc/linuxcampam/config.ini";
inline constexpr const char *USERS_DIR = "/etc/linuxcampam/users";
inline constexpr const char *MODELS_DIR = "/etc/linuxcampam/models";
inline constexpr const char *IR_EMITTER_PATH =
    "/usr/local/bin/linux-enable-ir-emitter";

inline constexpr size_t MAX_USERNAME_LENGTH = 32;
inline constexpr int SECURE_FILE_MODE = 0600;
inline constexpr double RGB_CHANNELS = 3.0;

// Camera & Auth constants
inline constexpr int CAMERA_WARMUP_FRAMES = 10;
inline constexpr int CAMERA_WARMUP_DELAY_MS = 100;
inline constexpr int CAMERA_AVERAGE_FRAMES = 5;
inline constexpr int IR_TRIGGER_DELAY_MS = 750;
inline constexpr int CAPTURE_RETRY_DELAY_S = 1;

// HDR Constants
inline constexpr int HDR_EXPOSURE_1 = 50;
inline constexpr int HDR_EXPOSURE_2 = 150;
inline constexpr int HDR_EXPOSURE_3 = 400;
inline constexpr int HDR_SETTLE_MS = 100;
inline constexpr int HDR_BIT_DEPTH = 255;
inline constexpr int CAMERA_RGB_WEIGHT = 40;
inline constexpr int DEFAULT_MIN_BRIGHTNESS = 40;
inline constexpr int CAPTURE_RETRY_ATTEMPTS = 3;

inline constexpr float MIRROR_THRESHOLD_DEFAULT = 0.6f; // detection confidence
inline constexpr int MIRROR_SIZE = 640;
inline constexpr int MIRROR_NMS = 5000; // keep top K bboxes before NMS .
} // namespace linuxcampam