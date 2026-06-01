#include "config.hpp"
#include "constants.hpp"
#include "logger.hpp"
#include "utils.hpp"

#include <charconv>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <optional>
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
inline std::optional<int> parse_int(std::string_view str) {
  if (str.empty())
    return std::nullopt;
  int out;
  auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), out);
  if (ec == std::errc{})
    return out;
  return std::nullopt;
}

// Safe float parsing (from_chars float support varies by compiler)
inline std::optional<float> parse_float(std::string_view str) {
  if (str.empty())
    return std::nullopt;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
  float out;
  auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), out);
  if (ec == std::errc{})
    return out;
  return std::nullopt;
#else
  try {
    return std::stof(std::string(str));
  } catch (...) {
    return std::nullopt;
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
  // General & Auth (Support both for backward compatibility, prefer [Auth])
  // General.threshold (Legacy) vs Auth.threshold
  std::string th_str = get("Auth.threshold");
  if (th_str.empty())
    th_str = get("General.threshold", std::to_string(DEFAULT_THRESHOLD));
  if (auto val = parse_float(th_str)) threshold = *val;

  std::string dt_str = get("Auth.detection_threshold");
  if (dt_str.empty())
    dt_str = get("General.detection_threshold",
                 std::to_string(DEFAULT_DETECTION_THRESHOLD));
  if (auto val = parse_float(dt_str)) detection_threshold = *val;

  std::string to_str = get("Auth.timeout_ms");
  if (to_str.empty())
    to_str = get("General.timeout_ms", std::to_string(DEFAULT_TIMEOUT_MS));
  if (auto val = parse_int(to_str)) timeout_ms = *val;

  // Auth Policy
  std::string method = get("Auth.policy");
  if (method.empty()) {
    // Fallback to General
    method = get("General.auth_method", "adaptive");
    if (ini.count("General.policy"))
      method = get("General.policy");
  }

  if (method == "strict_all" || method == "2fa" || method == "strict")
    policy = AuthPolicy::STRICT_ALL;
  else if (method == "lenient_any" || method == "1fa" || method == "lenient")
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
  // Can be in Auth or General
  std::string me_str = get("Auth.max_embeddings");
  if (me_str.empty())
    me_str =
        get("General.max_embeddings", std::to_string(DEFAULT_MAX_EMBEDDINGS));
  if (auto val = parse_int(me_str)) max_embeddings = *val;

  // Capture Settings (Global)
  if (ini.count("Capture.enroll_hdr"))
    enroll_hdr = get("Capture.enroll_hdr");
  if (ini.count("Capture.enroll_averaging"))
    enroll_averaging = (get("Capture.enroll_averaging") == "on" ||
                        get("Capture.enroll_averaging") == "true");
  if (auto val = parse_int(get("Capture.enroll_average_frames",
                std::to_string(DEFAULT_ENROLL_AVG_FRAMES))))
    enroll_average_frames = *val;

  if (ini.count("Capture.verify_averaging"))
    verify_averaging = (get("Capture.verify_averaging") == "on" ||
                        get("Capture.verify_averaging") == "true");
  if (auto val = parse_int(get("Capture.verify_average_frames",
                std::to_string(DEFAULT_VERIFY_AVG_FRAMES))))
    verify_average_frames = *val;

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
      std::string mb_str = get("Camera." + id + ".min_brightness", "0");
      if (auto val = parse_int(mb_str)) def.min_brightness = *val;
      def.mandatory = (get("Camera." + id + ".mandatory", "false") == "true");
      def.enroll_hdr = get("Camera." + id + ".enroll_hdr", "");
      def.enroll_averaging = get("Camera." + id + ".enroll_averaging", "");
      std::string eaf_str = get("Camera." + id + ".enroll_average_frames", "0");
      if (auto val = parse_int(eaf_str)) def.enroll_average_frames = *val;

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
        std::string hmb_str = get("Hardware.min_brightness",
                                  std::to_string(linuxcampam::DEFAULT_MIN_BRIGHTNESS));
        if (auto val = parse_int(hmb_str)) mb = *val;
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
  provider_priority.clear();
  std::string priority_str = get("Hardware.provider_priority", "");
  Logger::log(LogLevel::DEBUG, "Raw provider_priority: '" + priority_str + "'");

  if (!priority_str.empty()) {
    std::stringstream ss(priority_str);
    std::string segment;
    while (std::getline(ss, segment, ',')) {
      segment.erase(0, segment.find_first_not_of(" \t"));
      if (!segment.empty())
        provider_priority.push_back(segment);
    }
  }
  if (provider_priority.empty()) {
    Logger::log(LogLevel::DEBUG, "Using defaults for provider_priority");
    provider_priority = {"OpenCL", "CPU"};
  }

  for (const auto &p : provider_priority) {
    Logger::log(LogLevel::DEBUG, "Provider: " + p);
  }

  // Other settings
  save_success = (get("Storage.save_success_images") == "true");
  save_fail = (get("Storage.save_fail_images") == "true");
  if (auto val = parse_int(get("Performance.model_keep_alive_sec", "0"))) model_keep_alive_sec = *val;
  if (auto val = parse_int(get("Security.lockout_attempts", "5"))) lockout_attempts = *val;
  if (auto val = parse_int(get("Security.lockout_duration_sec", "300"))) lockout_duration_sec = *val;

  // Minimal UID (shared with PAM module)
  // We use int for parsing, then cast to uid_t
  int min_uid_int = linuxcampam::DEFAULT_MIN_UID;
  // Parse from [Security] section mostly
  // Note: PAM module has specific fallback logic to [General], we should map
  // similarly if needed or just trust [Security] as primary. Given PAM logic:
  // [Security] > First Found > Default. Here we explicitly look for
  // Security.min_uid first.
  std::string uid_str = get("Security.min_uid");
  if (uid_str.empty()) {
    uid_str = get("General.min_uid"); // Fallback check
  }

  // default string if both empty
  if (uid_str.empty()) {
    min_uid_int = linuxcampam::DEFAULT_MIN_UID;
  } else {
    if (auto val = parse_int(uid_str)) min_uid_int = *val;
  }

  // Safety check (similar to PAM module negative check, though parse_int
  // handles some)
  if (min_uid_int < 0)
    min_uid_int = linuxcampam::DEFAULT_MIN_UID;
  min_uid = static_cast<uid_t>(min_uid_int);
  gpu_flush = (get("Performance.gpu_flush", "on") == "on");
  if (auto val = parse_int(get("Performance.gpu_throttle_ms", "20"))) gpu_throttle_ms = *val;
}

std::string Configuration::toString() const {
  std::stringstream ss;
  ss << "=== Active Configuration ===\n\n";

  ss << "[General]\n";
  ss << "  Threshold: " << threshold << "\n";
  ss << "  Detection Threshold: " << detection_threshold << "\n";
  ss << "  Timeout: " << timeout_ms << " ms\n";
  ss << "  Auth Policy: ";
  switch (policy) {
  case AuthPolicy::ADAPTIVE:
    ss << "Adaptive (IR Strict, RGB Conditional)\n";
    break;
  case AuthPolicy::STRICT_ALL:
    ss << "Strict (All Cameras Must Match)\n";
    break;
  case AuthPolicy::LENIENT_ANY:
    ss << "Lenient (Any Camera Match)\n";
    break;
  default:
    ss << "Unknown (" << (int)policy << ")\n";
    break;
  }
  ss << "  Max Embeddings: " << max_embeddings << "\n\n";

  ss << "[Security]\n";
  ss << "  Lockout Attempts: " << lockout_attempts << "\n";
  ss << "  Lockout Duration: " << lockout_duration_sec << " s\n\n";

  ss << "[Cameras] (" << camera_defs.size() << " active)\n";
  for (const auto &cam : camera_defs) {
    ss << "  - ID: " << cam.id << "\n";
    ss << "    Path: " << cam.path << "\n";
    ss << "    Type: " << cam.type << "\n";
    ss << "    Mandatory: " << (cam.mandatory ? "Yes" : "No") << "\n";
    ss << "    Min Brightness: " << cam.min_brightness << "\n";
    if (!cam.enroll_hdr.empty()) {
      ss << "    Enroll HDR: " << cam.enroll_hdr << "\n";
    }
    if (cam.type == "ir") {
      std::string ir_ver =
          linuxcampam::getIREmitterVersion(ir_emitter_path.string());
      if (!ir_ver.empty()) {
        ss << "    IR Emitter Path: " << ir_emitter_path.string() << "\n";
        ss << "    IR Emitter Version: " << ir_ver << "\n";
      } else {
        ss << "    IR Emitter: Not Installed\n";
      }
    }
    ss << "\n";
  }

  ss << "[Performance]\n";
  ss << "  GPU Flush: " << (gpu_flush ? "On" : "Off") << "\n";
  ss << "  GPU Throttle: " << gpu_throttle_ms << " ms\n";
  ss << "  Provider Priority: ";
  for (size_t i = 0; i < provider_priority.size(); ++i) {
    ss << provider_priority[i];
    if (i < provider_priority.size() - 1)
      ss << " > ";
  }
  ss << "\n";

  return ss.str();
}
