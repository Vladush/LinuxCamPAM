#pragma once

#include "constants.hpp"
#include "utils.hpp"

#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>

namespace fs = std::filesystem;

class Configuration {
public:
  // Defaults
  // TODO: Move defaults to cpp or keep here? Keeping here for easy reference
  // akin to previous struct
  static constexpr float DEFAULT_THRESHOLD = 0.363f;
  static constexpr float DEFAULT_DETECTION_THRESHOLD = 0.9f;
  static constexpr int DEFAULT_TIMEOUT_MS = 3000;
  static constexpr int DEFAULT_MAX_EMBEDDINGS = 5;
  static constexpr int DEFAULT_ENROLL_AVG_FRAMES = 5;
  static constexpr int DEFAULT_VERIFY_AVG_FRAMES = 3;
  static constexpr int DEFAULT_LOCKOUT_ATTEMPTS = 5;
  static constexpr int DEFAULT_LOCKOUT_DURATION_SEC = 300;
  static constexpr int DEFAULT_GPU_THROTTLE_MS = 20;

  enum class AuthPolicy : std::uint8_t { STRICT_ALL, LENIENT_ANY, ADAPTIVE };
  enum class ProximitySensorMode : std::uint8_t { AUTO, ENABLED, DISABLED };

  struct CameraDefinition {
    std::string id{};
    std::string path{};
    std::string type{};
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
  std::vector<CameraDefinition> camera_defs{};

  bool save_success = false;
  bool save_fail = false;
  fs::path log_dir = "/var/log/linuxcampam/";
  std::string log_level = "info";
  std::string log_file = "";
  std::vector<std::string> provider_priority{};
  int model_keep_alive_sec = 0;

  std::string enroll_hdr = "auto";
  bool enroll_averaging = true;
  int enroll_average_frames = DEFAULT_ENROLL_AVG_FRAMES;
  bool verify_averaging = false;
  int verify_average_frames = DEFAULT_VERIFY_AVG_FRAMES;

  ProximitySensorMode proximity_sensor = ProximitySensorMode::AUTO;
  std::string proximity_sensor_id = "ITE8353";
  bool proximity_enforce = false;

  fs::path users_dir = linuxcampam::USERS_DIR;
  fs::path models_dir = linuxcampam::MODELS_DIR;
  fs::path ir_emitter_path = linuxcampam::IR_EMITTER_PATH;

  int lockout_attempts = DEFAULT_LOCKOUT_ATTEMPTS;
  int lockout_duration_sec = DEFAULT_LOCKOUT_DURATION_SEC;
  uid_t min_uid = linuxcampam::DEFAULT_MIN_UID;

  bool gpu_flush = true;
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
