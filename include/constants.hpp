#pragma once

#include <cstddef>

// Shared constants for LinuxCamPAM
namespace linuxcampam {
constexpr const char *SOCKET_PATH = "/run/linuxcampam/socket";
constexpr const char *CONFIG_PATH = "/etc/linuxcampam/config.ini";
constexpr const char *USERS_DIR = "/etc/linuxcampam/users";
constexpr const char *MODELS_DIR = "/etc/linuxcampam/models";
constexpr const char *IR_EMITTER_PATH =
    "/usr/local/bin/linux-enable-ir-emitter";

constexpr size_t MAX_USERNAME_LENGTH = 32;
constexpr int SECURE_FILE_MODE = 0600;
constexpr double RGB_CHANNELS = 3.0;

// Camera & Auth constants
constexpr int CAMERA_WARMUP_FRAMES = 10;
constexpr int CAMERA_WARMUP_DELAY_MS = 100;
constexpr int CAMERA_AVERAGE_FRAMES = 5;
constexpr int IR_TRIGGER_DELAY_MS = 750;
constexpr int CAPTURE_RETRY_DELAY_S = 1;

// HDR Constants
constexpr int HDR_EXPOSURE_1 = 50;
constexpr int HDR_EXPOSURE_2 = 150;
constexpr int HDR_EXPOSURE_3 = 400;
constexpr int HDR_SETTLE_MS = 100;
constexpr int HDR_BIT_DEPTH = 255;
constexpr int CAMERA_RGB_WEIGHT = 40;
constexpr int DEFAULT_MIN_BRIGHTNESS = 40;
constexpr int CAPTURE_RETRY_ATTEMPTS = 3;

constexpr float MIRROR_THRESHOLD_DEFAULT = 0.3f; // detection confidence
constexpr int MIRROR_SIZE = 320;
constexpr int MIRROR_NMS = 5000; // keep top K bboxes before NMS .
} // namespace linuxcampam
