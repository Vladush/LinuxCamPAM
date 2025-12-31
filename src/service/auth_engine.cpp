
#include "auth_engine.hpp"

#include "camera.hpp"
#include "constants.hpp"
#include "json.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <opencv2/core/ocl.hpp>
#include <regex>
#include <sstream>
#include <unordered_map>

// V4L2 for camera format detection
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

// Extract version from model filename (e.g. sface_2021dec)
inline std::string getModelVersion(const std::string &model_path) {
  fs::path p(model_path);
  std::string filename = p.stem().string();
  const std::string prefix = "face_recognition_";
  if (filename.rfind(prefix, 0) == 0) {
    return filename.substr(prefix.length());
  }
  return filename;
}

std::unordered_map<std::string, std::string>
parse_ini(const std::string &path) {
  std::unordered_map<std::string, std::string> result;
  std::ifstream file(path);
  if (!file.is_open())
    return result;

  std::string line, current_section;
  while (std::getline(file, line)) {
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
        result[current_section + "." + key] = val;
      }
    }
  }
  return result;
}

// Check if a video device is a valid capture device and classify its type
// Returns: "ir" for IR cameras (GREY/Y8/Y10 formats), "rgb" for color cameras,
// "" if not a capture device
std::string classifyCameraType(const std::string &device_path) {
  int fd = open(device_path.c_str(), O_RDONLY);
  if (fd < 0)
    return "";

  // Check if it's a capture device
  struct v4l2_capability cap = {};
  if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
    close(fd);
    return "";
  }

  // Must support video capture
  if (!(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE)) {
    close(fd);
    return "";
  }

  // Enumerate formats to detect IR vs RGB
  bool has_grey_format = false;
  bool has_color_format = false;

  struct v4l2_fmtdesc fmt = {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  while (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
    // IR cameras typically use grayscale formats
    if (fmt.pixelformat == V4L2_PIX_FMT_GREY ||
        fmt.pixelformat == V4L2_PIX_FMT_Y10 ||
        fmt.pixelformat == V4L2_PIX_FMT_Y12 ||
        fmt.pixelformat == V4L2_PIX_FMT_Y16) {
      has_grey_format = true;
    }
    // Color formats
    if (fmt.pixelformat == V4L2_PIX_FMT_MJPEG ||
        fmt.pixelformat == V4L2_PIX_FMT_YUYV ||
        fmt.pixelformat == V4L2_PIX_FMT_RGB24 ||
        fmt.pixelformat == V4L2_PIX_FMT_BGR24) {
      has_color_format = true;
    }
    fmt.index++;
  }

  close(fd);

  // Prefer classification: if has grey but no color, it's IR
  if (has_grey_format && !has_color_format)
    return "ir";
  if (has_color_format)
    return "rgb";
  if (has_grey_format)
    return "ir"; // Grey-only is likely IR

  return "generic"; // Unknown format, treat as generic
}

// Enumerate all video devices and return classified cameras
std::vector<std::pair<std::string, std::string>> enumerateCameras() {
  std::vector<std::pair<std::string, std::string>> cameras;

  if (!fs::exists("/dev"))
    return cameras;

  for (const auto &entry : fs::directory_iterator("/dev")) {
    std::string name = entry.path().filename().string();
    if (name.rfind("video", 0) == 0) {
      std::string type = classifyCameraType(entry.path().string());
      if (!type.empty()) {
        cameras.push_back({entry.path().string(), type});
      }
    }
  }

  // Sort by device path for consistent ordering
  std::sort(cameras.begin(), cameras.end());
  return cameras;
}

AuthEngine::AuthEngine() {}
AuthEngine::~AuthEngine() {}

bool AuthEngine::init(const std::string &config_path) {
  auto ini = parse_ini(config_path);

  auto get = [&ini](const std::string &key,
                    const std::string &def = "") -> std::string {
    auto it = ini.find(key);
    return it != ini.end() ? it->second : def;
  };

  config.threshold = std::stof(get("Auth.threshold", "0.4"));
  config.detection_threshold =
      std::stof(get("Auth.detection_threshold", "0.6"));
  config.timeout_ms = std::stoi(get("Auth.timeout_ms", "3000"));

  // Parse Policy
  std::string policy_str = get("Auth.policy", "adaptive");
  if (policy_str == "strict")
    config.policy = AuthPolicy::STRICT_ALL;
  else if (policy_str == "lenient")
    config.policy = AuthPolicy::LENIENT_ANY;
  else
    config.policy = AuthPolicy::ADAPTIVE;

  config.max_embeddings = std::stoi(get("Auth.max_embeddings", "5"));

  // Capture settings
  config.enroll_hdr = get("Capture.enroll_hdr", "auto");
  config.enroll_averaging = (get("Capture.enroll_averaging", "on") == "on");
  config.enroll_average_frames =
      std::stoi(get("Capture.enroll_average_frames", "5"));
  config.verify_averaging = (get("Capture.verify_averaging", "off") == "on");
  config.verify_average_frames =
      std::stoi(get("Capture.verify_average_frames", "3"));

  // Parse Paths
  config.users_dir = get("Paths.users_dir", config.users_dir);
  config.models_dir = get("Paths.models_dir", linuxcampam::MODELS_DIR);
  config.ir_emitter_path =
      get("Paths.ir_emitter_path", linuxcampam::IR_EMITTER_PATH);

  // Initialize model paths dynamic vars
  // Note: If user supplies full path in config in future, handle that.
  // For now assuming models_dir + filename.
  detection_model_path =
      config.models_dir + "/face_detection_yunet_2022mar.onnx";
  recognition_model_path =
      config.models_dir + "/face_recognition_sface_2021dec.onnx";

  // Parse Cameras
  std::string cam_names = get("Cameras.names", "");
  if (!cam_names.empty()) {
    std::stringstream ss(cam_names);
    std::string id;
    while (std::getline(ss, id, ',')) {
      // trim
      id.erase(0, id.find_first_not_of(" \t"));
      id.erase(id.find_last_not_of(" \t") + 1);
      if (id.empty())
        continue;

      CameraDefinition def;
      def.id = id;
      def.path = get("Camera." + id + ".path", "/dev/video0");
      def.type = get("Camera." + id + ".type", "generic");
      def.min_brightness =
          std::stoi(get("Camera." + id + ".min_brightness", "0"));
      std::string mand_str = get("Camera." + id + ".mandatory", "false");
      def.mandatory = (mand_str == "true");

      // Per-camera capture settings (empty = use global)
      def.enroll_hdr = get("Camera." + id + ".enroll_hdr", "");
      def.enroll_averaging = get("Camera." + id + ".enroll_averaging", "");
      std::string avg_frames =
          get("Camera." + id + ".enroll_average_frames", "0");
      def.enroll_average_frames = std::stoi(avg_frames);

      config.camera_defs.push_back(def);
    }
  } else {
    // Smart Auto-Detection Fallback
    std::string path_ir = get("Hardware.camera_path_ir", "");
    std::string path_rgb = get("Hardware.camera_path_rgb", "");

    bool explicit_legacy = (!path_ir.empty() || !path_rgb.empty());

    if (explicit_legacy) {
      // Backward compatibility for explicit old config keys
      // Note: We no longer fall back to hardcoded paths if one is missing
      if (!path_ir.empty())
        config.camera_defs.push_back({"ir", path_ir, "ir", 0, true});
      if (!path_rgb.empty())
        config.camera_defs.push_back(
            {"rgb", path_rgb, "rgb",
             std::stoi(get("Hardware.min_brightness", "40")), false});
    } else {
      // No config at all: Auto-detect using V4L2
      Logger::log(LogLevel::INFO, "Auto-detecting cameras via V4L2...");

      auto detected = enumerateCameras();

      if (detected.empty()) {
        Logger::log(LogLevel::ERROR,
                    "No cameras detected! Face authentication will not work.");
        Logger::log(
            LogLevel::ERROR,
            "Troubleshooting: Run 'v4l2-ctl --list-devices' to check cameras.");
        // Keep service running for hot-plug scenarios
      } else {
        // Classify detected cameras
        std::string ir_path, rgb_path;
        for (const auto &[path, type] : detected) {
          Logger::log(LogLevel::INFO,
                      "Detected: " + path + " (type: " + type + ")");
          if (type == "ir" && ir_path.empty()) {
            ir_path = path;
          } else if ((type == "rgb" || type == "generic") && rgb_path.empty()) {
            rgb_path = path;
          }
        }

        // Build camera definitions based on what was found
        if (!ir_path.empty() && !rgb_path.empty()) {
          Logger::log(LogLevel::INFO, "Detected Dual Setup (IR+RGB).");
          config.camera_defs.push_back({"ir", ir_path, "ir", 0, true});
          config.camera_defs.push_back({"rgb", rgb_path, "rgb", 40, false});
        } else if (!rgb_path.empty()) {
          Logger::log(LogLevel::INFO, "Detected Single RGB Setup.");
          config.camera_defs.push_back({"rgb", rgb_path, "rgb", 0, true});
        } else if (!ir_path.empty()) {
          Logger::log(LogLevel::INFO, "Detected Single IR Setup.");
          config.camera_defs.push_back({"ir", ir_path, "ir", 0, true});
        } else if (!detected.empty()) {
          // Has some camera but couldn't classify - use first one
          const auto &[path, type] = detected[0];
          Logger::log(LogLevel::WARN, "Could not classify cameras. Using " +
                                          path + " as generic.");
          config.camera_defs.push_back({"cam0", path, "generic", 0, true});
        }
      }
    }
  }

  std::string priority_str = get("Hardware.provider_priority", "");
  std::stringstream ss(priority_str);
  std::string segment;
  while (std::getline(ss, segment, ',')) {
    segment.erase(0, segment.find_first_not_of(" \t"));
    if (!segment.empty())
      config.provider_priority.push_back(segment);
  }
  if (config.provider_priority.empty())
    config.provider_priority = {"OpenCL", "OpenVINO", "CUDA", "CPU"};

  config.save_success = (get("Storage.save_success_images") == "true");
  config.save_fail = (get("Storage.save_fail_images") == "true");

  std::string ka_str = get("Performance.model_keep_alive_sec", "0");
  config.model_keep_alive_sec = std::stoi(ka_str);

  last_activity_ = std::chrono::steady_clock::now();

  // 2. Initialize Models (Delegated)
  return loadModels();
}

bool AuthEngine::loadModels() {
  if (detector && recognizer)
    return true; // Already loaded

  // Re-parse backend config in case priority changed or dynamic re-eval needed?
  // For now assuming config.provider_priority is static after init.
  int backend_id = cv::dnn::DNN_BACKEND_OPENCV;
  int target_id = cv::dnn::DNN_TARGET_CPU;

  for (const auto &prov : config.provider_priority) {
    if (prov == "CUDA") {
      if (cv::cuda::getCudaEnabledDeviceCount() > 0) {
        backend_id = cv::dnn::DNN_BACKEND_CUDA;
        target_id = cv::dnn::DNN_TARGET_CUDA;
        std::cout << "[AuthEngine] Selecting CUDA Backend." << std::endl;
        break;
      }
    } else if (prov == "OpenVINO") {
      backend_id = cv::dnn::DNN_BACKEND_INFERENCE_ENGINE;
      target_id = cv::dnn::DNN_TARGET_CPU;
      std::cout << "[AuthEngine] Selecting OpenVINO Backend." << std::endl;
      break;
    } else if (prov == "OpenCL") {
      if (cv::ocl::haveOpenCL()) {
        cv::ocl::setUseOpenCL(true);
        backend_id = cv::dnn::DNN_BACKEND_OPENCV;
        target_id = cv::dnn::DNN_TARGET_OPENCL;
        std::cout << "[AuthEngine] Selecting OpenCL Backend." << std::endl;
        // Log the OpenCL device name for assurance
        cv::ocl::Device dev = cv::ocl::Device::getDefault();
        std::cout << "[AuthEngine] Hardware Device: " << dev.name() << " "
                  << dev.version() << std::endl;
      } else {
        std::cerr << "[AuthEngine] WARNING: OpenCL requested but not "
                     "detected. Falling back to CPU."
                  << std::endl;
        backend_id = cv::dnn::DNN_BACKEND_OPENCV;
        target_id = cv::dnn::DNN_TARGET_CPU;
      }
      break;
    }
  }

  try {
    std::cout << "[AuthEngine] Loading Detector: " << detection_model_path
              << std::endl;
    std::cout << "[AuthEngine] Loading Recognizer: " << recognition_model_path
              << std::endl;

    detector = cv::FaceDetectorYN::create(
        detection_model_path, "", cv::Size(320, 320),
        config.detection_threshold, 0.3f, 5000, backend_id, target_id);

    recognizer = cv::FaceRecognizerSF::create(recognition_model_path, "",
                                              backend_id, target_id);

    // Cameras are lightweight, "active_cameras" structs can be maintained,
    // but maybe camera connection should be re-verified?
    // For now, Camera object holds a persistent path. `Camera` ctor doesn't
    // open stream until `capture`. So active_cameras list is fine to persist.
    if (active_cameras.empty()) {
      active_cameras.clear();
      for (const auto &def : config.camera_defs) {
        ActiveCamera ac;
        ac.config = def;
        Logger::log(LogLevel::INFO, "Initializing Camera: " + def.id + " (" +
                                        def.type + ") at " + def.path);
        ac.cam = std::make_unique<Camera>(def.path, def.type == "ir",
                                          config.ir_emitter_path);
        active_cameras.push_back(std::move(ac));
      }
    }
  } catch (const cv::Exception &e) {
    Logger::log(LogLevel::ERROR,
                "Error loading models: " + std::string(e.what()));
    return false;
  }

  last_activity_ = std::chrono::steady_clock::now();
  return true;
}

void AuthEngine::unloadModels() {
  if (detector) {
    Logger::log(LogLevel::INFO, "Unloading AI models to save RAM.");
    detector.release();
    recognizer.release();
  }
  // Optional: cv::cuda::resetDevice()? Usually not safe if multi-threaded.
}

bool AuthEngine::ensureModelsLoaded() {
  if (!detector) {
    Logger::log(LogLevel::INFO, "Wake up! Reloading models...");
    return loadModels();
  }
  // Refresh activity
  last_activity_ = std::chrono::steady_clock::now();
  return true;
}

bool AuthEngine::performMaintenance() {
  // Check if unload is needed
  // TODO: maybe add configurable grace period?
  if (config.model_keep_alive_sec > 0 && detector) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(now - last_activity_)
            .count();
    if (elapsed > config.model_keep_alive_sec) {
      unloadModels();
      return true;
    }
  }
  return false;
}

void AuthEngine::fallbackToCPU() {
  Logger::log(LogLevel::WARN, "Attempting fallback to CPU backend...");
  try {
    detector = cv::FaceDetectorYN::create(
        detection_model_path, "", cv::Size(320, 320),
        config.detection_threshold, 0.3f, 5000, cv::dnn::DNN_BACKEND_OPENCV,
        cv::dnn::DNN_TARGET_CPU);
    recognizer = cv::FaceRecognizerSF::create(recognition_model_path, "",
                                              cv::dnn::DNN_BACKEND_OPENCV,
                                              cv::dnn::DNN_TARGET_CPU);
    Logger::log(LogLevel::INFO, "Successfully switched to CPU backend.");
  } catch (const cv::Exception &e) {
    Logger::log(LogLevel::ERROR,
                "Failed to switch to CPU backend: " + std::string(e.what()));
  }
}

bool AuthEngine::isValidUsername(const std::string &username) {
  // Allow alphanumeric, underscore, dot, dash.
  // Prevent path traversal characters like "/" or "..".
  // Max length 32 for sanity.
  if (username.length() > 32 || username.empty())
    return false;
  static const std::regex re("^[a-zA-Z0-9_\\.-]+$");
  return std::regex_match(username, re);
}

cv::Mat AuthEngine::captureFrame(Camera *cam) {
  if (!cam)
    return cv::Mat();
  return cam->capture();
}

double AuthEngine::calculateBrightness(const cv::Mat &frame) {
  if (frame.empty())
    return 0.0;
  cv::Scalar means = cv::mean(frame);
  return (means[0] + means[1] + means[2]) / 3.0;
}

bool AuthEngine::verifyUser(const std::string &username) {
  if (!ensureModelsLoaded()) {
    std::cerr << "[AuthEngine] CRITICAL: Failed to load models!" << std::endl;
    return false;
  }
  if (!isValidUsername(username)) {
    std::cerr << "[AuthEngine] Security Warn: Invalid username string: "
              << username << std::endl;
    return false;
  }
  std::string user_file =
      std::string(config.users_dir) + "/" + username + ".json";
  if (!fs::exists(user_file))
    return false;

  std::ifstream f(user_file);
  json j;
  f >> j;

  int participants = 0;
  int successes = 0;
  int failures = 0;

  Logger::log(LogLevel::INFO, "Verifying user " + username + " with policy " +
                                  std::to_string((int)config.policy));

  for (auto &ac : active_cameras) {
    std::string id = ac.config.id;
    // Capture
    cv::Mat frame = captureFrame(ac.cam.get());

    // Participation Check
    if (frame.empty()) {
      std::cout << "[AuthEngine] Camera " << id << " failed to capture."
                << std::endl;
      if (config.policy == AuthPolicy::STRICT_ALL)
        return false;
      if (config.policy == AuthPolicy::ADAPTIVE && ac.config.mandatory) {
        Logger::log(LogLevel::WARN,
                    "Critical Mandatory Camera " + id + " failed. Abort.");
        return false;
      }
      continue;
    }

    if (ac.config.min_brightness > 0) {
      double b = calculateBrightness(frame);
      if (b < ac.config.min_brightness) {
        if (config.policy == AuthPolicy::ADAPTIVE && ac.config.mandatory) {
          Logger::log(LogLevel::WARN,
                      "Mandatory Camera " + id + " is too dark (" +
                          std::to_string(b) + " < " +
                          std::to_string(ac.config.min_brightness) +
                          "). Failing.");
          return false;
        }
        Logger::log(LogLevel::DEBUG,
                    "Camera " + id + " too dark (" + std::to_string(b) + " < " +
                        std::to_string(ac.config.min_brightness) +
                        "). Skipping.");
        continue;
      }
    }

    participants++;

    // Load embeddings (multi-format first, then legacy)
    std::string emb_array_key = "embeddings_" + ac.config.type;
    std::string emb_key = "embedding_" + ac.config.type;

    std::vector<std::vector<float>> all_embeddings;

    if (j.contains(emb_array_key) && j[emb_array_key].is_array()) {
      for (const auto &entry : j[emb_array_key]) {
        if (entry.contains("data")) {
          all_embeddings.push_back(entry["data"].get<std::vector<float>>());
        }
      }
    } else if (j.contains(emb_key)) {
      all_embeddings.push_back(j[emb_key].get<std::vector<float>>());
    }

    if (all_embeddings.empty()) {
      Logger::log(LogLevel::WARN, "No embeddings found for " + ac.config.type);
      failures++;
      if (config.save_fail)
        cv::imwrite(config.log_dir + "fail_missing_" + id + "_" + username +
                        ".jpg",
                    frame);
      continue;
    }

    // Detect faces
    cv::Mat faces;
    detector->setInputSize(frame.size());
    detector->detect(frame, faces);

    bool match = false;
    if (faces.rows >= 1) {
      cv::Mat aligned_face, curr_emb;
      float best_score = 0.0f;

      // For each detected face, compare against ALL stored embeddings
      for (int i = 0; i < faces.rows; i++) {
        recognizer->alignCrop(frame, faces.row(i), aligned_face);
        recognizer->feature(aligned_face, curr_emb);

        for (const auto &stored_vec : all_embeddings) {
          cv::Mat stored_emb(1, stored_vec.size(), CV_32F,
                             const_cast<float *>(stored_vec.data()));
          float score = cosine_similarity(curr_emb, stored_emb);
          if (score > best_score)
            best_score = score;
        }
      }

      Logger::log(LogLevel::INFO,
                  id + " Score: " + std::to_string(best_score) +
                      " (threshold: " + std::to_string(config.threshold) +
                      ", embeddings: " + std::to_string(all_embeddings.size()) +
                      ")");
      if (best_score >= config.threshold) {
        match = true;
        Logger::log(LogLevel::INFO, id + " MATCH.");
      } else {
        Logger::log(LogLevel::INFO, id + " MISMATCH: score below threshold.");
      }
    } else {
      Logger::log(LogLevel::WARN, id + " NO_FACE_DETECTED in frame.");
    }

    if (match) {
      successes++;
      if (config.save_success)
        cv::imwrite(config.log_dir + "success_" + id + "_" + username + ".jpg",
                    frame);
    } else {
      failures++;
      if (config.save_fail)
        cv::imwrite(config.log_dir + "fail_" + id + "_" + username + ".jpg",
                    frame);
    }
  }

  if (participants == 0) {
    Logger::log(LogLevel::WARN, "No cameras verified (all failed or skipped).");
    return false;
  }

  if (config.policy == AuthPolicy::STRICT_ALL)
    return failures == 0;
  if (config.policy == AuthPolicy::LENIENT_ANY)
    return successes > 0;

  return failures == 0;
}

AuthResult AuthEngine::verifyUserWithDetails(const std::string &username) {
  AuthResult result;
  result.success = false;
  result.best_score = 0.0f;

  if (!ensureModelsLoaded()) {
    result.reason = "Failed to load models";
    return result;
  }
  if (!isValidUsername(username)) {
    result.reason = "Invalid username";
    return result;
  }
  std::string user_file =
      std::string(config.users_dir) + "/" + username + ".json";
  if (!fs::exists(user_file)) {
    result.reason = "User not enrolled";
    return result;
  }

  std::ifstream f(user_file);
  json j;
  f >> j;

  int participants = 0;
  int successes = 0;
  int failures = 0;
  bool any_no_face = false;
  float overall_best_score = 0.0f;

  for (auto &ac : active_cameras) {
    std::string id = ac.config.id;
    cv::Mat frame = captureFrame(ac.cam.get());

    if (frame.empty()) {
      if (config.policy == AuthPolicy::STRICT_ALL ||
          (config.policy == AuthPolicy::ADAPTIVE && ac.config.mandatory)) {
        result.reason = "Camera " + id + " failed to capture";
        return result;
      }
      continue;
    }

    participants++;

    // Load embeddings
    std::string emb_array_key = "embeddings_" + ac.config.type;
    std::string emb_key = "embedding_" + ac.config.type;
    std::vector<std::vector<float>> all_embeddings;

    if (j.contains(emb_array_key) && j[emb_array_key].is_array()) {
      for (const auto &entry : j[emb_array_key]) {
        if (entry.contains("data")) {
          all_embeddings.push_back(entry["data"].get<std::vector<float>>());
        }
      }
    } else if (j.contains(emb_key)) {
      all_embeddings.push_back(j[emb_key].get<std::vector<float>>());
    }

    if (all_embeddings.empty()) {
      failures++;
      continue;
    }

    // Detect faces
    cv::Mat faces;
    detector->setInputSize(frame.size());
    detector->detect(frame, faces);

    bool match = false;
    if (faces.rows >= 1) {
      cv::Mat aligned_face, curr_emb;
      float best_score = 0.0f;

      for (int i = 0; i < faces.rows; i++) {
        recognizer->alignCrop(frame, faces.row(i), aligned_face);
        recognizer->feature(aligned_face, curr_emb);

        for (const auto &stored_vec : all_embeddings) {
          cv::Mat stored_emb(1, stored_vec.size(), CV_32F,
                             const_cast<float *>(stored_vec.data()));
          float score = cosine_similarity(curr_emb, stored_emb);
          if (score > best_score)
            best_score = score;
        }
      }

      if (best_score > overall_best_score)
        overall_best_score = best_score;

      if (best_score >= config.threshold) {
        match = true;
        successes++;
      } else {
        failures++;
      }
    } else {
      any_no_face = true;
      failures++;
    }
  }

  result.best_score = overall_best_score;

  if (participants == 0) {
    result.reason = "No cameras participated";
    return result;
  }

  bool auth_ok = false;
  if (config.policy == AuthPolicy::STRICT_ALL)
    auth_ok = (failures == 0);
  else if (config.policy == AuthPolicy::LENIENT_ANY)
    auth_ok = (successes > 0);
  else
    auth_ok = (failures == 0);

  if (auth_ok) {
    result.success = true;
    return result;
  }

  // Determine failure reason
  if (any_no_face) {
    result.reason = "No face detected";
  } else if (overall_best_score > 0) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "Face mismatch (score: " << overall_best_score << ")";
    result.reason = oss.str();
  } else {
    result.reason = "Authentication failed";
  }
  return result;
}

std::pair<bool, std::string>
AuthEngine::enrollUser(const std::string &username) {
  if (!ensureModelsLoaded())
    return {false, "Failed to load AI models."};
  if (!isValidUsername(username)) {
    std::cerr << "[AuthEngine] Security Warn: Invalid username string: "
              << username << std::endl;
    return {false, "Invalid username (security restriction)."};
  }

  // Load existing user file or create new
  std::string user_file =
      std::string(config.users_dir) + "/" + username + ".json";
  json j;
  if (fs::exists(user_file)) {
    std::ifstream f(user_file);
    f >> j;
  } else {
    j["username"] = username;
    j["created"] = std::time(nullptr);
  }

  Logger::log(LogLevel::INFO, "Enrolling user " + username + " across " +
                                  std::to_string(active_cameras.size()) +
                                  " cameras.");

  for (auto &ac : active_cameras) {
    std::string id = ac.config.id;
    Logger::log(LogLevel::DEBUG, "Capturing from " + id + "...");

    // Use enhanced capture for enrollment
    // Per-camera settings override global if set
    std::string use_hdr = !ac.config.enroll_hdr.empty() ? ac.config.enroll_hdr
                                                        : config.enroll_hdr;
    bool use_averaging = !ac.config.enroll_averaging.empty()
                             ? (ac.config.enroll_averaging == "on")
                             : config.enroll_averaging;
    int avg_frames = (ac.config.enroll_average_frames > 0)
                         ? ac.config.enroll_average_frames
                         : config.enroll_average_frames;

    cv::Mat frame;
    if (use_hdr == "on" ||
        (use_hdr == "auto" && ac.cam->supportsManualExposure())) {
      frame = ac.cam->captureHDR();
    } else if (use_averaging) {
      frame = ac.cam->captureAveraged(avg_frames);
    } else {
      frame = ac.cam->capture();
    }

    if (frame.empty()) {
      Logger::log(LogLevel::ERROR, "Camera " + id + " failed. Enroll aborted.");
      return {false, "Camera " + id + " failed (empty frame)."};
    }

    detector->setInputSize(frame.size());
    cv::Mat faces;
    detector->detect(frame, faces);

    if (faces.rows != 1) {
      std::string err = "Found " + std::to_string(faces.rows) + " faces in " +
                        id + ". Expecting exactly 1.";
      Logger::log(LogLevel::WARN, "Enroll failed: " + err);
      if (config.save_fail) {
        cv::imwrite(config.log_dir + "failed_enroll_" + id + "_" + username +
                        ".jpg",
                    frame);
      }
      return {false, err};
    }

    cv::Mat aligned, emb;
    recognizer->alignCrop(frame, faces.row(0), aligned);
    recognizer->feature(aligned, emb);

    std::vector<float> vec;
    emb.reshape(1, 1).copyTo(vec);

    // Store as pending embedding (will be finalized by setLabel)
    std::string pending_key = "_pending_" + ac.config.type;
    j[pending_key] = vec;
  }

  Logger::log(LogLevel::INFO, "Saving pending enrollment...");
  fs::create_directories(config.users_dir);
  std::ofstream out(user_file);
  out << j.dump(4);
  return {true, "Success"};
}

bool AuthEngine::setLabel(const std::string &username,
                          const std::string &label) {
  if (!isValidUsername(username))
    return false;

  std::string user_file =
      std::string(config.users_dir) + "/" + username + ".json";
  if (!fs::exists(user_file))
    return false;

  std::ifstream f(user_file);
  json j;
  f >> j;
  f.close();

  bool updated = false;
  for (auto &ac : active_cameras) {
    std::string pending_key = "_pending_" + ac.config.type;
    std::string emb_array_key = "embeddings_" + ac.config.type;

    if (j.contains(pending_key)) {
      auto embedding_data = j[pending_key];

      // Initialize array if not exists
      if (!j.contains(emb_array_key)) {
        j[emb_array_key] = json::array();

        // Migrate old single-embedding format if exists
        std::string old_key = "embedding_" + ac.config.type;
        if (j.contains(old_key)) {
          json old_entry;
          old_entry["label"] = "default";
          old_entry["data"] = j[old_key];
          old_entry["created"] = j.value("created", std::time(nullptr));
          j[emb_array_key].push_back(old_entry);
          j.erase(old_key);
        }
      }

      // Check max embeddings limit
      if (config.max_embeddings > 0 &&
          j[emb_array_key].size() >=
              static_cast<size_t>(config.max_embeddings)) {
        // Find and replace same label, or reject
        bool found = false;
        for (auto &entry : j[emb_array_key]) {
          if (entry["label"] == label) {
            entry["data"] = embedding_data;
            entry["created"] = std::time(nullptr);
            entry["model_version"] = getModelVersion(recognition_model_path);
            found = true;
            break;
          }
        }
        if (!found) {
          Logger::log(LogLevel::WARN,
                      "Max embeddings (" +
                          std::to_string(config.max_embeddings) +
                          ") reached for " + username);
          return false;
        }
      } else {
        // Find existing label to overwrite, or add new
        bool found = false;
        for (auto &entry : j[emb_array_key]) {
          if (entry["label"] == label) {
            entry["data"] = embedding_data;
            entry["created"] = std::time(nullptr);
            entry["model_version"] = getModelVersion(recognition_model_path);
            found = true;
            break;
          }
        }
        if (!found) {
          json new_entry;
          new_entry["label"] = label;
          new_entry["data"] = embedding_data;
          new_entry["created"] = std::time(nullptr);
          new_entry["model_version"] = getModelVersion(recognition_model_path);
          j[emb_array_key].push_back(new_entry);
        }
      }

      j.erase(pending_key);
      updated = true;
    }
  }

  if (updated) {
    std::ofstream out(user_file);
    out << j.dump(4);
    Logger::log(LogLevel::INFO, "Set label '" + label + "' for " + username);
  }
  return updated;
}

bool AuthEngine::trainUser(const std::string &username,
                           const std::string &label, bool create_new) {
  if (!ensureModelsLoaded())
    return false;
  if (!isValidUsername(username)) {
    Logger::log(LogLevel::WARN,
                "Security Warn: Invalid username string: " + username);
    return false;
  }
  std::string user_file =
      std::string(config.users_dir) + "/" + username + ".json";
  if (!fs::exists(user_file))
    return false;

  std::ifstream f(user_file);
  json j;
  f >> j;
  f.close();

  bool updated_any = false;

  for (auto &ac : active_cameras) {
    std::string id = ac.config.id;
    std::string emb_array_key = "embeddings_" + ac.config.type;
    std::string emb_key = "embedding_" + ac.config.type;

    cv::Mat frame = captureFrame(ac.cam.get());
    if (frame.empty()) {
      Logger::log(LogLevel::WARN, "Train: Camera " + id + " failed capture.");
      continue;
    }

    cv::Mat faces;
    detector->setInputSize(frame.size());
    detector->detect(frame, faces);
    if (faces.rows != 1) {
      Logger::log(LogLevel::WARN, "Train: Expected 1 face, found " +
                                      std::to_string(faces.rows));
      continue;
    }

    cv::Mat aligned, new_emb;
    recognizer->alignCrop(frame, faces.row(0), aligned);
    recognizer->feature(aligned, new_emb);

    std::vector<float> new_vec;
    new_emb.reshape(1, 1).copyTo(new_vec);

    // Initialize array if needed
    if (!j.contains(emb_array_key)) {
      j[emb_array_key] = json::array();
      // Migrate legacy format
      if (j.contains(emb_key)) {
        json entry;
        entry["label"] = "default";
        entry["data"] = j[emb_key];
        entry["created"] = j.value("created", std::time(nullptr));
        j[emb_array_key].push_back(entry);
        j.erase(emb_key);
      }
    }

    if (create_new) {
      // Add as new embedding
      if (config.max_embeddings > 0 &&
          j[emb_array_key].size() >=
              static_cast<size_t>(config.max_embeddings)) {
        Logger::log(LogLevel::WARN, "Max embeddings reached for " + username);
        return false;
      }
      json entry;
      entry["label"] = label.empty()
                           ? "trained_" + std::to_string(std::time(nullptr))
                           : label;
      entry["data"] = new_vec;
      entry["created"] = std::time(nullptr);
      j[emb_array_key].push_back(entry);
      Logger::log(LogLevel::INFO, "Train: Added new embedding '" +
                                      entry["label"].get<std::string>() + "'");
      updated_any = true;
    } else {
      // Refine existing label (average)
      bool found = false;
      for (auto &entry : j[emb_array_key]) {
        if (entry["label"] == label) {
          std::vector<float> old_vec = entry["data"].get<std::vector<float>>();
          cv::Mat old_emb(1, old_vec.size(), CV_32F, old_vec.data());
          cv::Mat avg = old_emb + new_emb;
          cv::normalize(avg, avg);
          std::vector<float> avg_vec;
          avg.reshape(1, 1).copyTo(avg_vec);
          entry["data"] = avg_vec;
          entry["created"] = std::time(nullptr);
          found = true;
          Logger::log(LogLevel::INFO,
                      "Train: Refined embedding '" + label + "'");
          updated_any = true;
          break;
        }
      }
      if (!found) {
        // Create new if label doesn't exist
        json entry;
        entry["label"] = label;
        entry["data"] = new_vec;
        entry["created"] = std::time(nullptr);
        j[emb_array_key].push_back(entry);
        Logger::log(LogLevel::INFO,
                    "Train: Created new embedding '" + label + "'");
        updated_any = true;
      }
    }
  }

  if (updated_any) {
    std::ofstream out(user_file);
    out << j.dump(4);
  }
  return updated_any;
}

std::vector<std::string>
AuthEngine::listEmbeddings(const std::string &username) {
  std::vector<std::string> labels;
  if (!isValidUsername(username))
    return labels;

  std::string user_file =
      std::string(config.users_dir) + "/" + username + ".json";
  if (!fs::exists(user_file))
    return labels;

  std::ifstream f(user_file);
  json j;
  f >> j;

  for (auto &ac : active_cameras) {
    std::string emb_array_key = "embeddings_" + ac.config.type;
    if (j.contains(emb_array_key) && j[emb_array_key].is_array()) {
      for (const auto &entry : j[emb_array_key]) {
        if (entry.contains("label")) {
          std::string lbl = entry["label"].get<std::string>();
          if (std::find(labels.begin(), labels.end(), lbl) == labels.end()) {
            labels.push_back(lbl);
          }
        }
      }
    }
    // Check legacy format
    std::string emb_key = "embedding_" + ac.config.type;
    if (j.contains(emb_key)) {
      if (std::find(labels.begin(), labels.end(), "default") == labels.end()) {
        labels.push_back("default (legacy)");
      }
    }
  }
  return labels;
}

bool AuthEngine::removeEmbedding(const std::string &username,
                                 const std::string &label) {
  if (!isValidUsername(username))
    return false;

  std::string user_file =
      std::string(config.users_dir) + "/" + username + ".json";
  if (!fs::exists(user_file))
    return false;

  std::ifstream f(user_file);
  json j;
  f >> j;
  f.close();

  bool removed = false;
  for (auto &ac : active_cameras) {
    std::string emb_array_key = "embeddings_" + ac.config.type;
    if (j.contains(emb_array_key) && j[emb_array_key].is_array()) {
      auto &arr = j[emb_array_key];
      for (auto it = arr.begin(); it != arr.end();) {
        if ((*it).contains("label") && (*it)["label"] == label) {
          it = arr.erase(it);
          removed = true;
        } else {
          ++it;
        }
      }
    }
  }

  if (removed) {
    std::ofstream out(user_file);
    out << j.dump(4);
    Logger::log(LogLevel::INFO,
                "Removed embedding '" + label + "' for " + username);
  }
  return removed;
}

bool AuthEngine::testCameraAndAuth() {
  if (!ensureModelsLoaded())
    return false;
  bool any_ok = false;
  Logger::log(LogLevel::INFO,
              "Testing " + std::to_string(active_cameras.size()) + " cameras.");

  for (auto &ac : active_cameras) {
    std::string id = ac.config.id;
    Logger::log(LogLevel::INFO, "Testing Camera " + id + "...");
    cv::Mat frame = captureFrame(ac.cam.get());
    if (!frame.empty()) {
      detector->setInputSize(frame.size());
      cv::Mat faces;
      detector->detect(frame, faces);
      Logger::log(LogLevel::INFO, "  -> Capture OK. Faces detected: " +
                                      std::to_string(faces.rows));
      any_ok = true;
    } else {
      Logger::log(LogLevel::ERROR, "  -> Capture Failed.");
    }
  }

  return any_ok;
}
