#include "config.hpp"

#include "logger.hpp"
#include "utils.hpp"

#include <charconv>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>
#include <unordered_map>

namespace {

// Helper to parse INI stream
std::unordered_map<std::string, std::string>
parse_ini_stream(std::istream &input) {
  std::unordered_map<std::string, std::string> result;
  std::string line, current_section;
  while (std::getline(input, line)) {
    // Trim
    line.erase(0, line.find_first_not_of(" \t"));
    if (line.empty() || line[0] == ';')
      continue;
    auto last = line.find_last_not_of(" \t");
    if (last != std::string::npos)
      line.erase(last + 1);

    if (line[0] == '[' && line.back() == ']') {
      current_section = line.substr(1, line.size() - 2);
    } else {
      size_t eq = line.find('=');
      if (eq != std::string::npos) {
        std::string key = line.substr(0, eq);
        key.erase(key.find_last_not_of(" \t") + 1);
        std::string val = line.substr(eq + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        std::string full_key = current_section;
        full_key += '.';
        full_key += key;
        result[full_key] = val;
      }
    }
  }
  return result;
}

// Safe int parsing without exceptions
inline bool parse_int(const std::string &str, int &out) {
  if (str.empty())
    return false;
  auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), out);
  return ec == std::errc{};
}

// Safe float parsing (from_chars float support varies by compiler)
inline bool parse_float(const std::string &str, float &out) {
  if (str.empty())
    return false;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
  auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), out);
  return ec == std::errc{};
#else
  try {
    out = std::stof(str);
    return true;
  } catch (...) {
    return false;
  }
#endif
}

} // namespace

bool Configuration::load(const fs::path &config_path,
                         const linuxcampam::ICameraBackend *backend) {
  std::ifstream file(config_path);
  if (!file.is_open()) {
    // It's valid to load nothing/defaults if file missing?
    // Existing code just returned empty map.
    // We'll proceed with empty stream or just call parse with empty.
    std::stringstream ss;
    return load(ss, backend);
  }
  return load(file, backend);
}

bool Configuration::load(std::istream &input,
                         const linuxcampam::ICameraBackend *backend) {
  parse_ini_into_self(input, backend);
  return true;
}

void Configuration::parse_ini_into_self(
    std::istream &input, const linuxcampam::ICameraBackend *backend) {
  auto ini = parse_ini_stream(input);
  auto get = [&](const std::string &key, const std::string &def = "") {
    return ini.count(key) ? ini.at(key) : def;
  };

  // Logic ported from AuthEngine::init
  // General
  parse_float(get("General.threshold", std::to_string(DEFAULT_THRESHOLD)),
              threshold);
  parse_float(get("General.detection_threshold",
                  std::to_string(DEFAULT_DETECTION_THRESHOLD)),
              detection_threshold);
  parse_int(get("General.timeout_ms", std::to_string(DEFAULT_TIMEOUT_MS)),
            timeout_ms);

  // Auth Policy
  std::string method =
      get("General.auth_method", "adaptive"); // Default fallback
  // Check for explicit policy override
  if (ini.count("General.policy"))
    method = get("General.policy");

  if (method == "strict_all" || method == "2fa")
    policy = AuthPolicy::STRICT_ALL;
  else if (method == "lenient_any" || method == "1fa")
    policy = AuthPolicy::LENIENT_ANY;
  else
    policy = AuthPolicy::ADAPTIVE;

  // Paths
  if (ini.count("Paths.users_dir"))
    users_dir = get("Paths.users_dir");
  if (ini.count("Paths.models_dir"))
    models_dir = get("Paths.models_dir");
  if (ini.count("Paths.ir_emitter_path"))
    ir_emitter_path = get("Paths.ir_emitter_path");

  // Limits
  parse_int(
      get("General.max_embeddings", std::to_string(DEFAULT_MAX_EMBEDDINGS)),
      max_embeddings);

  // Cameras
  std::string cam_names = get("Cameras.names", "");
  if (!cam_names.empty()) {
    std::stringstream ss(cam_names);
    std::string id;
    while (std::getline(ss, id, ',')) {
      // Trim logic
      id.erase(0, id.find_first_not_of(" \t"));
      id.erase(id.find_last_not_of(" \t") + 1);
      if (id.empty())
        continue;

      CameraDefinition def;
      def.id = id;
      def.path = get("Camera." + id + ".path", "/dev/video0");
      def.type = get("Camera." + id + ".type", "generic");
      try {
        def.min_brightness =
            std::stoi(get("Camera." + id + ".min_brightness", "0"));
      } catch (...) {
      }
      def.mandatory = (get("Camera." + id + ".mandatory", "false") == "true");
      def.enroll_hdr = get("Camera." + id + ".enroll_hdr", "");
      def.enroll_averaging = get("Camera." + id + ".enroll_averaging", "");
      try {
        def.enroll_average_frames =
            std::stoi(get("Camera." + id + ".enroll_average_frames", "0"));
      } catch (...) {
      }

      camera_defs.push_back(def);
    }
  } else {
    // Auto detection logic
    std::string path_ir = get("Hardware.camera_path_ir", "");
    std::string path_rgb = get("Hardware.camera_path_rgb", "");
    if (!path_ir.empty() || !path_rgb.empty()) {
      if (!path_ir.empty())
        camera_defs.push_back({"ir", path_ir, "ir", 0, true});
      if (!path_rgb.empty()) {
        int mb = linuxcampam::DEFAULT_MIN_BRIGHTNESS;
        try {
          mb = std::stoi(
              get("Hardware.min_brightness",
                  std::to_string(linuxcampam::DEFAULT_MIN_BRIGHTNESS)));
        } catch (...) {
        }
        camera_defs.push_back({"rgb", path_rgb, "rgb", mb, false});
      }
    } else {
      // Full auto detect
      std::vector<std::pair<std::string, std::string>> detected;
      if (backend) {
        detected = linuxcampam::enumerateCameras(*backend);
      } else {
        detected = linuxcampam::enumerateCameras();
      }

      if (!detected.empty()) {
        std::string ir_p, rgb_p;
        for (auto &[device_path, type] : detected) {
          if (type == "ir" && ir_p.empty())
            ir_p = device_path;
          else if ((type == "rgb" || type == "generic") && rgb_p.empty())
            rgb_p = device_path;
        }
        if (!ir_p.empty() && !rgb_p.empty()) {
          camera_defs.push_back({"ir", ir_p, "ir", 0, true});
          camera_defs.push_back(
              {"rgb", rgb_p, "rgb", linuxcampam::CAMERA_RGB_WEIGHT, false});
        } else if (!rgb_p.empty()) {
          camera_defs.push_back({"rgb", rgb_p, "rgb", 0, true});
        } else if (!ir_p.empty()) {
          camera_defs.push_back({"ir", ir_p, "ir", 0, true});
        } else {
          camera_defs.push_back(
              {"cam0", detected[0].first, "generic", 0, true});
        }
      } else {
        log_warn("No cameras detected during config load.");
      }
    }
  }

  // Hardware/Provider
  std::string priority_str = get("Hardware.provider_priority", "");
  if (!priority_str.empty()) {
    std::stringstream ss(priority_str);
    std::string segment;
    while (std::getline(ss, segment, ',')) {
      segment.erase(0, segment.find_first_not_of(" \t"));
      if (!segment.empty())
        provider_priority.push_back(segment);
    }
  }
  if (provider_priority.empty())
    provider_priority = {"OpenCL", "OpenVINO", "CUDA", "CPU"};

  // Other settings
  save_success = (get("Storage.save_success_images") == "true");
  save_fail = (get("Storage.save_fail_images") == "true");
  parse_int(get("Performance.model_keep_alive_sec", "0"), model_keep_alive_sec);
  parse_int(get("Security.lockout_attempts", "5"), lockout_attempts);
  parse_int(get("Security.lockout_duration_sec", "300"), lockout_duration_sec);
  gpu_flush = (get("Performance.gpu_flush", "on") == "on");
  parse_int(get("Performance.gpu_throttle_ms", "20"), gpu_throttle_ms);
}

std::string Configuration::toString() const {
  std::stringstream ss;
  ss << "Config[Thres=" << threshold << ", Pol=" << (int)policy
     << ", Cams=" << camera_defs.size() << "]";
  return ss.str();
}
