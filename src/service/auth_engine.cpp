
#include "auth_engine.hpp"

#include "camera.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "json.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <linux/videodev2.h>
#include <opencv2/core/ocl.hpp>
#include <sstream>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

namespace fs = std::filesystem;
using json = nlohmann::json;

inline void gpuSync(bool do_flush, int delay_ms) {
  if (do_flush && cv::ocl::useOpenCL()) {
    try {
      cv::ocl::finish();
    } catch (const cv::Exception &e) {
      // Driver might be unstable, log warning but keep running
      std::cerr << "GPU Sync warning: " << e.what() << std::endl;
    }
  }
  if (delay_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  }
}

// Extract version from model filename (e.g. sface_2021dec)
inline std::string getModelVersion(const fs::path &model_path) {
  std::string filename = model_path.stem().string();
  const std::string prefix = "face_recognition_";
  if (filename.rfind(prefix, 0) == 0) {
    return filename.substr(prefix.length());
  }
  return filename;
}

// Check for explicit legacy behavior, otherwise skip

AuthEngine::AuthEngine() {}
AuthEngine::~AuthEngine() {}

bool AuthEngine::init(const fs::path &config_path) {
  if (!config.load(config_path)) {
    log_warn("Failed to load config from " + config_path.string() +
             ". Using defaults.");
  }

  // Initialize last activity
  last_activity_ = std::chrono::steady_clock::now();

  // Initialize model paths dynamic vars
  // Note: If user supplies full path in config in future, handle that.
  // For now assuming models_dir + filename.
  detection_model_path =
      config.models_dir / "face_detection_yunet_2022mar.onnx";
  recognition_model_path =
      config.models_dir / "face_recognition_sface_2021dec.onnx";

  // Initialize Cameras if any
  // Note: config.load() already populated config.camera_defs via auto-detection
  // if needed.
  if (active_cameras.empty()) {
    active_cameras.clear();
    for (const auto &def : config.camera_defs) {
      ActiveCamera ac;
      ac.config = def; // Copy config
      log_info("Initializing Camera: " + def.id + " (" + def.type + ") at " +
               def.path);
      // Assuming Camera accepts fs::path for ir emitter
      ac.cam = std::make_unique<Camera>(def.path, def.type == "ir",
                                        config.ir_emitter_path);
      active_cameras.push_back(std::move(ac));
    }
  }

  return loadModels();
}

bool AuthEngine::loadModels() {
  if (detector && recognizer)
    return true; // Already loaded

  // Re-parse backend config in case priority changed or dynamic re-eval
  // needed? For now assuming config.provider_priority is static after init.
  int backend_id = cv::dnn::DNN_BACKEND_OPENCV;
  int target_id = cv::dnn::DNN_TARGET_CPU;

  for (const auto &prov : config.provider_priority) {
    if (prov == "CUDA") {
      if (cv::cuda::getCudaEnabledDeviceCount() > 0) {
        backend_id = cv::dnn::DNN_BACKEND_CUDA;
        target_id = cv::dnn::DNN_TARGET_CUDA;
        log_info("Selecting CUDA Backend.");
        break;
      }
    } else if (prov == "OpenVINO") {
      backend_id = cv::dnn::DNN_BACKEND_INFERENCE_ENGINE;
      target_id = cv::dnn::DNN_TARGET_CPU;
      log_info("Selecting OpenVINO Backend.");
      break;
    } else if (prov == "OpenCL") {
      if (cv::ocl::haveOpenCL()) {
        cv::ocl::setUseOpenCL(true);
        backend_id = cv::dnn::DNN_BACKEND_OPENCV;
        target_id = cv::dnn::DNN_TARGET_OPENCL;
        log_info("Selecting OpenCL Backend.");
        // Log the OpenCL device name for assurance
        cv::ocl::Device dev = cv::ocl::Device::getDefault();
        log_info("Hardware Device: " + dev.name() + " " + dev.version());
      } else {
        log_warn("OpenCL requested but not "
                 "detected. Falling back to CPU.");
        backend_id = cv::dnn::DNN_BACKEND_OPENCV;
        target_id = cv::dnn::DNN_TARGET_CPU;
      }
      break;
    }
  }

  try {
    log_info("Loading Detector: " + detection_model_path.string());
    log_info("Loading Recognizer: " + recognition_model_path.string());

    detector = cv::FaceDetectorYN::create(
        detection_model_path, "",
        cv::Size(linuxcampam::MIRROR_SIZE, linuxcampam::MIRROR_SIZE),
        config.detection_threshold, linuxcampam::MIRROR_THRESHOLD_DEFAULT,
        linuxcampam::MIRROR_NMS, backend_id, target_id);

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
        log_info("Initializing Camera: " + def.id + " (" + def.type + ") at " +
                 def.path);
        ac.cam = std::make_unique<Camera>(def.path, def.type == "ir",
                                          config.ir_emitter_path.string());
        active_cameras.push_back(std::move(ac));
      }
    }
  } catch (const cv::Exception &e) {
    log_error("Error loading models: " + std::string(e.what()));
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
        detection_model_path, "",
        cv::Size(linuxcampam::MIRROR_SIZE, linuxcampam::MIRROR_SIZE),
        config.detection_threshold, linuxcampam::MIRROR_THRESHOLD_DEFAULT,
        linuxcampam::MIRROR_NMS, cv::dnn::DNN_BACKEND_OPENCV,
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

bool AuthEngine::isValidUsername(std::string_view username) {
  if (username.empty() || username.length() > linuxcampam::MAX_USERNAME_LENGTH)
    return false;

  // Hidden files
  if (username.front() == '.')
    return false;

  // Detect Path Traversal ("..")
  if (std::adjacent_find(username.begin(), username.end(), [](char a, char b) {
        return a == '.' && b == '.';
      }) != username.end()) {
    return false;
  }

  // Strict allowlist (a-z, A-Z, 0-9, _, ., -, $)
  return std::all_of(username.begin(), username.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' ||
           c == '$';
  });
}

cv::Mat AuthEngine::captureFrame(ICamera *cam) {
  if (!cam)
    return cv::Mat();
  return cam->capture();
}

double AuthEngine::calculateBrightness(const cv::Mat &frame) {
  if (frame.empty())
    return 0.0;
  cv::Scalar means = cv::mean(frame);
  return (means[0] + means[1] + means[2]) / linuxcampam::RGB_CHANNELS;
}

bool AuthEngine::verifyUser(const std::string &username) {
  if (!ensureModelsLoaded()) {
    std::cerr << "[AuthEngine] CRITICAL: Failed to load models!" << std::endl;
    return false;
  }
  if (!isValidUsername(username)) {
    log_warn("Security Warn: Invalid username string: " + username);
    return false;
  }
  if (isUserLockedOut(username)) {
    Logger::log(LogLevel::WARN, "User " + username + " is locked out");
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

  log_info("Verifying user " + username + " with policy " +
           std::to_string((int)config.policy));
  log_debug("Required successes: " + std::to_string(config.threshold));

  for (auto &ac : active_cameras) {
    std::string id = ac.config.id;
    // Capture
    cv::Mat frame = captureFrame(ac.cam.get());

    // Participation Check
    if (frame.empty()) {
      std::cout << "[AuthEngine] Camera " << id << " failed to capture."
                << std::endl;
      if (config.policy == Configuration::AuthPolicy::STRICT_ALL)
        return false;
      if (config.policy == Configuration::AuthPolicy::ADAPTIVE &&
          ac.config.mandatory) {
        Logger::log(LogLevel::WARN,
                    "Critical Mandatory Camera " + id + " failed. Abort.");
        return false;
      }
      continue;
    }

    if (ac.config.min_brightness > 0) {
      double b = calculateBrightness(frame);
      if (b < ac.config.min_brightness) {
        if (config.policy == Configuration::AuthPolicy::ADAPTIVE &&
            ac.config.mandatory) {
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
      std::string msg = "No embeddings found for ";
      msg += ac.config.type;
      Logger::log(LogLevel::WARN, msg);
      failures++;
      if (config.save_fail) {
        std::string fail_filename = config.log_dir + "fail_missing_";
        fail_filename += id;
        fail_filename += "_";
        fail_filename += username;
        fail_filename += ".jpg";
        cv::imwrite(fail_filename, frame);
      }
      continue;
    }

    // Detect faces
    cv::Mat faces;
    detector->setInputSize(frame.size());
    detector->detect(frame, faces);
    gpuSync(config.gpu_flush, config.gpu_throttle_ms);

    bool match = false;
    if (faces.rows >= 1) {
      // Log if multiple faces found
      if (faces.rows > 1) {
        log_warn("Multiple faces detected (" + std::to_string(faces.rows) +
                 "), using largest.");
      }

      cv::Mat aligned_face, curr_emb;
      // Use largest face (row 0)
      recognizer->alignCrop(frame, faces.row(0), aligned_face);
      recognizer->feature(aligned_face, curr_emb);
      gpuSync(config.gpu_flush, config.gpu_throttle_ms);

      double best_score = 0.0;
      match = false;

      auto best_match_it = std::max_element(
          all_embeddings.begin(), all_embeddings.end(),
          [&](const auto &a, const auto &b) {
            cv::Mat emb_a(1, static_cast<int>(a.size()), CV_32F,
                          const_cast<float *>(a.data()));
            cv::Mat emb_b(1, static_cast<int>(b.size()), CV_32F,
                          const_cast<float *>(b.data()));
            return recognizer->match(curr_emb, emb_a,
                                     cv::FaceRecognizerSF::FR_COSINE) <
                   recognizer->match(curr_emb, emb_b,
                                     cv::FaceRecognizerSF::FR_COSINE);
          });

      if (best_match_it != all_embeddings.end()) {
        cv::Mat ref_emb(1, static_cast<int>(best_match_it->size()), CV_32F,
                        const_cast<float *>(best_match_it->data()));
        best_score = recognizer->match(curr_emb, ref_emb,
                                       cv::FaceRecognizerSF::FR_COSINE);
      }

      log_debug("  -> Best Match Score: " + std::to_string(best_score));

      if (best_score >= config.threshold) {
        match = true;
      }

      if (match) {
        std::string msg = "Camera ";
        msg += id;
        msg += " MATCH (Score: ";
        msg += std::to_string(best_score);
        msg += ")";
        Logger::log(LogLevel::INFO, msg);
        successes++;
        if (config.save_success) {
          std::string success_filename = config.log_dir + "success_";
          success_filename += id;
          success_filename += "_";
          success_filename += username;
          success_filename += ".jpg";
          cv::imwrite(success_filename, frame);
        }
      } else {
        std::string msg = "Camera ";
        msg += id;
        msg += " NO MATCH (Best: ";
        msg += std::to_string(best_score);
        msg += ")";
        Logger::log(LogLevel::WARN, msg);
        failures++;
        // Save fail image
        if (config.save_fail) {
          std::string filename = config.log_dir;
          filename += "fail_mismatched_";
          filename += id;
          filename += "_";
          filename += username;
          filename += ".jpg";
          cv::imwrite(filename, frame);
          log_debug("Saved mismatch image to: " + filename);
        }
      }
    } else {
      std::string msg = "Camera ";
      msg += id;
      msg += " NO_FACE_DETECTED in frame.";
      log_warn(msg);
      failures++;
      if (config.save_fail) {
        std::string fail_filename = config.log_dir + "fail_";
        fail_filename += id;
        fail_filename += "_";
        fail_filename += username;
        fail_filename += ".jpg";
        cv::imwrite(fail_filename, frame);
      }
    }
  }

  if (participants == 0) {
    Logger::log(LogLevel::WARN, "No cameras verified (all failed or skipped).");
    return false;
  }

  if (config.policy == Configuration::AuthPolicy::STRICT_ALL)
    return failures == 0 && successes > 0;

  // LENIENT or ADAPTIVE (mandatory checks already passed if we got here)
  bool success = (successes > 0);
  recordAuthAttempt(username, success);
  return success;
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
  if (isUserLockedOut(username)) {
    result.reason = "User locked out";
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
      if (config.policy == Configuration::AuthPolicy::STRICT_ALL ||
          (config.policy == Configuration::AuthPolicy::ADAPTIVE &&
           ac.config.mandatory)) {
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
    gpuSync(config.gpu_flush, config.gpu_throttle_ms);

    if (faces.rows >= 1) {
      cv::Mat aligned_face, curr_emb;
      float best_score = 0.0f;

      for (int i = 0; i < faces.rows; i++) {
        recognizer->alignCrop(frame, faces.row(i), aligned_face);
        recognizer->feature(aligned_face, curr_emb);
        gpuSync(config.gpu_flush, config.gpu_throttle_ms);

        if (!all_embeddings.empty()) {
          auto best_it =
              std::max_element(all_embeddings.begin(), all_embeddings.end(),
                               [&](const auto &a, const auto &b) {
                                 cv::Mat emb_a(1, a.size(), CV_32F,
                                               const_cast<float *>(a.data()));
                                 cv::Mat emb_b(1, b.size(), CV_32F,
                                               const_cast<float *>(b.data()));
                                 return cosine_similarity(curr_emb, emb_a) <
                                        cosine_similarity(curr_emb, emb_b);
                               });

          cv::Mat best_ref_emb(1, static_cast<int>(best_it->size()), CV_32F,
                               const_cast<float *>(best_it->data()));
          float score = cosine_similarity(curr_emb, best_ref_emb);
          if (score > best_score)
            best_score = score;
        }
      }

      if (best_score > overall_best_score)
        overall_best_score = best_score;

      if (best_score >= config.threshold) {
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
  if (config.policy == Configuration::AuthPolicy::STRICT_ALL)
    auth_ok = (failures == 0 && successes > 0);
  else
    auth_ok = (successes > 0);

  if (auth_ok) {
    result.success = true;
    return result;
  }

  // Determine failure reason
  if (any_no_face) {
    result.reason = "No face detected";
  } else if (overall_best_score > 0) {
    std::ostringstream oss = {};
    oss << std::fixed << std::setprecision(2);
    oss << "Face mismatch (score: " << overall_best_score << ")";
    result.reason = oss.str();
  } else {
    result.reason = "Authentication failed";
  }
  recordAuthAttempt(username, result.success);
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
    gpuSync(config.gpu_flush, config.gpu_throttle_ms);

    if (faces.rows != 1) {
      std::string err = "Found ";
      err += std::to_string(faces.rows);
      err += " faces in ";
      err += id;
      err += ". Expecting exactly 1.";
      Logger::log(LogLevel::WARN, "Enroll failed: " + err);
      if (config.save_fail) {
        std::string fail_filename = config.log_dir + "failed_enroll_";
        fail_filename += id;
        fail_filename += "_";
        fail_filename += username;
        fail_filename += ".jpg";
        cv::imwrite(fail_filename, frame);
      }
      return {false, err};
    }

    cv::Mat aligned, emb;
    recognizer->alignCrop(frame, faces.row(0), aligned);
    recognizer->feature(aligned, emb);
    gpuSync(config.gpu_flush, config.gpu_throttle_ms);

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
  out.close();
  chmod(user_file.c_str(),
        linuxcampam::SECURE_FILE_MODE); // Restrict to root-only
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
    out.close();
    chmod(user_file.c_str(),
          linuxcampam::SECURE_FILE_MODE); // Restrict to root-only
    Logger::log(LogLevel::INFO, "Set label '" + label + "' for " + username);
  }
  return updated;
}

bool AuthEngine::trainUser(const std::string &username,
                           const std::string &label, bool create_new) {
  if (!ensureModelsLoaded())
    return false;
  if (!isValidUsername(username)) {
    log_warn("Security Warn: Invalid username string: " + username);
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
      log_warn("Train: Camera " + id + " failed capture.");
      continue;
    }

    cv::Mat faces;
    detector->setInputSize(frame.size());
    detector->detect(frame, faces);
    gpuSync(config.gpu_flush, config.gpu_throttle_ms);
    if (faces.rows != 1) {
      log_warn("Train: Expected 1 face, found " + std::to_string(faces.rows));
      continue;
    }

    cv::Mat aligned, new_emb;
    recognizer->alignCrop(frame, faces.row(0), aligned);
    recognizer->feature(aligned, new_emb);
    gpuSync(config.gpu_flush, config.gpu_throttle_ms);

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
          cv::Mat old_emb(1, static_cast<int>(old_vec.size()), CV_32F,
                          old_vec.data());
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
    out.close();
    chmod(user_file.c_str(),
          linuxcampam::SECURE_FILE_MODE); // Restrict to root-only
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
    out.close();
    chmod(user_file.c_str(),
          linuxcampam::SECURE_FILE_MODE); // Restrict to root-only
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

bool AuthEngine::isUserLockedOut(const std::string &username) {
  if (config.lockout_attempts <= 0)
    return false;

  std::scoped_lock lock(lockout_mutex_);
  auto it = lockout_map_.find(username);
  if (it == lockout_map_.end())
    return false;

  if (std::chrono::steady_clock::now() < it->second.lockout_until)
    return true;

  return false;
}

void AuthEngine::recordAuthAttempt(const std::string &username, bool success) {
  if (config.lockout_attempts <= 0)
    return;

  std::scoped_lock lock(lockout_mutex_);
  auto &state = lockout_map_[username];

  if (success) {
    state.failed_attempts = 0;
    state.lockout_until = {};
  } else {
    state.failed_attempts++;
    if (state.failed_attempts >= config.lockout_attempts) {
      state.lockout_until = std::chrono::steady_clock::now() +
                            std::chrono::seconds(config.lockout_duration_sec);
      Logger::log(LogLevel::WARN, username + " locked out");
    }
  }
}

std::string AuthEngine::getConfigString() const {
  return config.toString();
}
