#pragma once

#include "constants.hpp"
#include "utils.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

class Configuration {
public:
  // Defaults
  // TODO: Move defaults to cpp or keep here? Keeping here for easy reference
  // akin to previous struct
  static constexpr float DEFAULT_THRESHOLD = 0.363f;
  // Lowered from 0.9 to 0.6: IR cameras (e.g. Lenovo, Dell) produce low-contrast
  // grayscale images where YuNet confidence scores rarely exceed 0.7.
  // A threshold of 0.9 works well for RGB webcams but effectively blocks
  // all detection on IR streams. Override via detection_threshold in config.ini.
  static constexpr float DEFAULT_DETECTION_THRESHOLD = 0.6f;
  static constexpr int DEFAULT_TIMEOUT_MS = 3000;
  static constexpr int DEFAULT_MAX_EMBEDDINGS = 5;
  static constexpr int DEFAULT_ENROLL_AVG_FRAMES = 5;
  static constexpr int DEFAULT_VERIFY_AVG_FRAMES = 3;
  static constexpr int DEFAULT_LOCKOUT_ATTEMPTS = 5;
  static constexpr int DEFAULT_LOCKOUT_DURATION_SEC = 300;
  static constexpr int DEFAULT_GPU_THROTTLE_MS = 20;

  enum class AuthPolicy { STRICT_ALL, LENIENT_ANY, ADAPTIVE };

  struct CameraDefinition {
    std::string id;
    std::string path;
    std::string type; // "ir", "rgb"
    int min_brightness = 0;
    bool mandatory = false;
    std::string enroll_hdr = "";
    std::string enroll_averaging = "";
    int enroll_average_frames = 0;
  };

  // Data Members
  float threshold = DEFAULT_THRESHOLD;
  float detection_threshold = DEFAULT_DETECTION_THRESHOLD;
  int timeout_ms = DEFAULT_TIMEOUT_MS;
  int max_embeddings = DEFAULT_MAX_EMBEDDINGS;

  AuthPolicy policy = AuthPolicy::ADAPTIVE;
  std::vector<CameraDefinition> camera_defs;

  bool save_success = false;
  bool save_fail = false;
  std::string log_dir = "/var/log/linuxcampam/";
  std::vector<std::string> provider_priority;
  int model_keep_alive_sec = 0;

  std::string enroll_hdr = "auto";
  bool enroll_averaging = true;
  int enroll_average_frames = DEFAULT_ENROLL_AVG_FRAMES;
  bool verify_averaging = false;
  int verify_average_frames = DEFAULT_VERIFY_AVG_FRAMES;

  fs::path users_dir = linuxcampam::USERS_DIR;
  fs::path models_dir = linuxcampam::MODELS_DIR;
  fs::path ir_emitter_path = linuxcampam::IR_EMITTER_PATH;

  int lockout_attempts = DEFAULT_LOCKOUT_ATTEMPTS;
  int lockout_duration_sec = DEFAULT_LOCKOUT_DURATION_SEC;
  uid_t min_uid = linuxcampam::DEFAULT_MIN_UID;

  bool gpu_flush = false;
  int gpu_throttle_ms = DEFAULT_GPU_THROTTLE_MS;

  // Methods
  [[nodiscard]] bool load(const fs::path &config_path,
                          const linuxcampam::ICameraBackend *backend = nullptr);
  [[nodiscard]] bool load(std::istream &input,
                          const linuxcampam::ICameraBackend *backend = nullptr);
  [[nodiscard]] std::string toString() const;

private:
  void
  parse_ini_into_self(std::istream &input,
                      const linuxcampam::ICameraBackend *backend = nullptr);
};
