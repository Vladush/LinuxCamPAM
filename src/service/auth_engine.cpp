
#include "auth_engine.hpp"

#include "camera.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "json.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <linux/videodev2.h>
#include <opencv2/core/ocl.hpp>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <unistd.h>

#include <optional>

namespace fs = std::filesystem;
using json = nlohmann::json;

// Helper for loading user json safely
static std::optional<json> loadUserJsonSafe(const fs::path& user_file) {
  std::ifstream f(user_file);
  if (!f.is_open()) return std::nullopt;
  try {
    json j;
    f >> j;
    return j;
  } catch (const json::parse_error& e) {
    log_error("JSON parse error in " + user_file.string() + ": " + e.what());
    return std::nullopt;
  }
}

inline void gpuSync(bool do_flush, int delay_ms) {
  if (do_flush && cv::ocl::useOpenCL()) {
    try {
      cv::ocl::finish();
    } catch (const cv::Exception &e) {
      // Driver might be unstable, log warning but keep running
      std::cerr << "GPU Sync warning: " << e.what() << '\n';
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

// A default factory is set up here to create real Camera objects.
// Tests can override this later using setCameraFactory().
AuthEngine::AuthEngine() {
  camera_factory_ = [this](const Configuration::CameraDefinition &def) {
    return std::make_unique<Camera>(def.path, def.type == "ir",
                                    config.ir_emitter_path.string());
  };
}
AuthEngine::~AuthEngine() {}

void AuthEngine::setCameraFactory(CameraFactory factory) {
  camera_factory_ = std::move(factory);
}

bool AuthEngine::init(const fs::path &config_path) {
  if (!config.load(config_path)) {
    log_warn("Failed to load config from " + config_path.string() +
             ". Using defaults.");
  }

  // Initialize last activity
  last_activity_ = std::chrono::steady_clock::now();

  // Initialize model paths dynamic vars
  // Note: If user supplies full path in config in future, handle that.

  // Default model paths (System Install)
  const std::string DEFAULT_DETECTOR_PATH =
      "/usr/share/linuxcampam/models/face_detection_yunet_2023mar.onnx";
  const std::string DEFAULT_RECOGNIZER_PATH =
      "/usr/share/linuxcampam/models/face_recognition_sface_2021dec.onnx";

  detection_model_path = DEFAULT_DETECTOR_PATH;
  recognition_model_path = DEFAULT_RECOGNIZER_PATH;

  // Initialize Cameras if any
  // Note: config.load() already populated config.camera_defs via auto-detection
  // if needed.
  initializeActiveCameras();

  return loadModels();
}

void AuthEngine::initializeActiveCameras() {
  if (active_cameras.empty()) {
    active_cameras.reserve(config.camera_defs.size());
    std::transform(
        config.camera_defs.begin(), config.camera_defs.end(),
        std::back_inserter(active_cameras), [&](const auto &def) {
          ActiveCamera ac;
          ac.config = def; // Copy config
          log_info("Initializing Camera: " + def.id + " (" + def.type +
                   ") at " + def.path);
          if (def.type == "ir") {
            std::string ir_ver = linuxcampam::getIREmitterVersion(
                config.ir_emitter_path.string());
            if (!ir_ver.empty()) {
              log_info("IR Emitter Engine: " + config.ir_emitter_path.string() +
                       " (" + ir_ver + ")");
            } else {
              log_info("IR Emitter Engine: Not Installed");
            }
          }
          ac.cam = camera_factory_(def);
          return ac;
        });
  }
}

bool AuthEngine::loadModels() {
  if (detector && recognizer)
    return true; // Already loaded

  // Re-parse backend config in case priority changed or dynamic re-eval
  // needed? For now assuming config.provider_priority is static after init.
  int backend_id = cv::dnn::DNN_BACKEND_OPENCV;
  int target_id = cv::dnn::DNN_TARGET_CPU;

  for (std::string_view prov : config.provider_priority) {
    if (prov == "OpenCL") {
      if (cv::ocl::haveOpenCL()) {
        cv::ocl::setUseOpenCL(true);
        backend_id = cv::dnn::DNN_BACKEND_OPENCV;
        target_id = cv::dnn::DNN_TARGET_OPENCL;
        log_info("Selecting OpenCL Backend.");
        // Log the OpenCL device name for assurance
        const cv::ocl::Device &dev = cv::ocl::Device::getDefault();
        log_info("Hardware Device: " + dev.name() + " " + dev.version());
        break;
      } else {
        log_warn("OpenCL requested but not detected. Trying next provider "
                 "fallback.");
      }
    } else if (prov == "CPU") {
      backend_id = cv::dnn::DNN_BACKEND_OPENCV;
      target_id = cv::dnn::DNN_TARGET_CPU;
      log_info("Selecting CPU Backend.");
      break;
    } else {
      log_warn("Unsupported or unrecognized backend requested: " +
               std::string(prov));
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
    initializeActiveCameras();
  } catch (const cv::Exception &e) {
    log_error("Error loading models: " + std::string(e.what()));
    return false;
  }

  last_activity_ = std::chrono::steady_clock::now();
  return true;
}

void AuthEngine::unloadModels() {
  if (detector) {
    log_info("Unloading AI models to save RAM.");
    detector.release();
    recognizer.release();
  }
  // Optional: cv::cuda::resetDevice()? Usually not safe if multi-threaded.
}

bool AuthEngine::ensureModelsLoaded() {
  if (!detector) {
    log_info("Wake up! Reloading models...");
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
  log_warn("Attempting fallback to CPU backend...");
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
    log_info("Successfully switched to CPU backend.");
  } catch (const cv::Exception &e) {
    log_error("Failed to switch to CPU backend: " + std::string(e.what()));
  }
}

int AuthEngine::generateEmbedding(const cv::Mat &frame,
                                  std::vector<float> &out_embedding,
                                  cv::Mat &out_aligned_face) {
  if (frame.empty())
    return 0;

  auto run_inference = [&]() {
    cv::Mat faces;
    log_debug("Profiling: About to detect (dynamic size)");

    detector->setInputSize(frame.size());
    detector->detect(frame, faces);

    int num_faces = faces.rows;
    if (num_faces == 0) {
      log_warn("Profiling: Detection complete. 0 faces found above threshold (" +
                  std::to_string(config.detection_threshold) +
                  "). If using IR camera, try lowering detection_threshold to 0.5 in config.ini.");
    } else {
      constexpr int FACE_SCORE_INDEX = 14;
      float best_score = faces.at<float>(0, FACE_SCORE_INDEX);
      log_info("Profiling: Detection complete. Faces: " + std::to_string(num_faces) +
                  " | Best score: " + std::to_string(best_score) +
                  " | Threshold: " + std::to_string(config.detection_threshold));
    }
    gpuSync(config.gpu_flush, config.gpu_throttle_ms);

    if (num_faces < 1)
      return 0;

    log_debug("Profiling: About to align and recognize");

    cv::Mat emb_mat;
    // Use largest face (row 0)
    recognizer->alignCrop(frame, faces.row(0), out_aligned_face);
    recognizer->feature(out_aligned_face, emb_mat);

    log_debug("Profiling: Recognition complete");
    gpuSync(config.gpu_flush, config.gpu_throttle_ms);

    if (!emb_mat.empty()) {
      emb_mat.reshape(1, 1).copyTo(out_embedding);
    }

    return num_faces;
  };

  try {
    return run_inference();
  } catch (const cv::Exception &e) {
    if (cv::ocl::useOpenCL()) {
      log_warn("OpenCL failure, switching to CPU: " + std::string(e.what()));
      cv::ocl::setUseOpenCL(false);
      fallbackToCPU();
      return run_inference();
    }
    throw;
  }
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

AuthResult AuthEngine::verifyUserCore(std::string_view username,
                                      const PerCameraCallback &callback) {
  AuthResult result;
  result.success = false;
  result.best_score = 0.0f;

  if (!linuxcampam::isValidUsername(username)) {
    result.reason = "Invalid username";
    if (callback)
      callback("security", cv::Mat(), false, 0, result.reason);
    return result;
  }
  if (isUserLockedOut(username)) {
    result.reason = "User locked out";
    return result;
  }
  if (!ensureModelsLoaded()) {
    result.reason = "Failed to load models";
    return result;
  }
  fs::path user_file = config.users_dir / (std::string(username) + ".json");
  auto j_opt = loadUserJsonSafe(user_file);
  if (!j_opt) {
    result.reason = "User not enrolled or corrupt data";
    return result;
  }
  json j = *j_opt;

  int participants = 0;
  int successes = 0;
  int failures = 0;
  bool any_no_face = false;
  float overall_best_score = 0.0f;

  log_info("Verifying user " + std::string(username) + " with policy " +
           std::to_string(static_cast<int>(config.policy)));

  for (auto &ac : active_cameras) {
    std::string id = ac.config.id;
    cv::Mat frame = captureFrame(ac.cam.get());

    if (frame.empty()) {
      if (callback)
        callback(id, frame, false, 0.0f, "Capture failed");

      if (config.policy == Configuration::AuthPolicy::STRICT_ALL) {
        result.reason = "Camera " + id + " failed to capture";
        return result;
      }
      if (config.policy == Configuration::AuthPolicy::ADAPTIVE &&
          ac.config.mandatory) {
        log_warn("Critical Mandatory Camera " + id + " failed. Abort.");
        result.reason = "Mandatory Camera " + id + " failed";
        return result;
      }
      continue;
    }

    // Brightness Check
    if (ac.config.min_brightness > 0) {
      double b = calculateBrightness(frame);
      if (b < ac.config.min_brightness) {
        std::string msg = "Too dark (" + std::to_string(b) + " < " +
                          std::to_string(ac.config.min_brightness) + ")";
        if (callback)
          callback(id, frame, false, 0.0f, msg);

        if (config.policy == Configuration::AuthPolicy::ADAPTIVE &&
            ac.config.mandatory) {
          log_warn("Mandatory Camera " + id + " is too dark. Failing.");
          result.reason = "Mandatory Camera " + id + " too dark";
          return result;
        }
        continue; // Skip dark camera
      }
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
      if (callback)
        callback(id, frame, false, 0.0f, "No embeddings found");
      failures++;
      continue;
    }

    // Detect faces and generate embedding
    std::vector<float> curr_emb_vec;
    cv::Mat aligned_face;
    int num_faces = generateEmbedding(frame, curr_emb_vec, aligned_face);

    if (num_faces >= 1) {
      if (num_faces > 1) {
        log_warn("Multiple faces detected (" + std::to_string(num_faces) +
                 "), using largest.");
      }

      cv::Mat curr_emb(1, static_cast<int>(curr_emb_vec.size()), CV_32F);
      std::copy(curr_emb_vec.begin(), curr_emb_vec.end(),
                curr_emb.ptr<float>());

      float best_camera_score = 0.0f;

      // Match against all embeddings
      // Using direct match logic for efficiency
      for (auto &emb_vec : all_embeddings) {
        // A non-const reference is used to avoid const_cast since cv::Mat constructor requires a non-const pointer.
        // cv::FaceRecognizerSF::match treats inputs as read-only. Avoids allocations per embedding.
        cv::Mat emb_ref(1, static_cast<int>(emb_vec.size()), CV_32F, emb_vec.data());
        float score = static_cast<float>(recognizer->match(
            curr_emb, emb_ref, cv::FaceRecognizerSF::FR_COSINE));
        if (score > best_camera_score)
          best_camera_score = score;
      }

      if (best_camera_score > overall_best_score)
        overall_best_score = best_camera_score;

      bool match = (best_camera_score >= config.threshold);
      if (match) {
        successes++;
      } else {
        failures++;
      }

      if (callback)
        callback(id, frame, match, best_camera_score,
                 match ? "MATCH" : "NO MATCH");

    } else {
      any_no_face = true;
      failures++;
      if (callback)
        callback(id, frame, false, 0.0f, "NO_FACE_DETECTED");
    }
  }

  result.best_score = overall_best_score;

  if (participants == 0) {
    log_warn("No cameras verified (all failed or skipped).");
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
  } else {
    if (overall_best_score > 0) {
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(2);
      oss << "Face mismatch (score: " << overall_best_score << ")";
      result.reason = oss.str();
      if (any_no_face) {
        result.reason += " (some cameras failed detection)";
      }
    } else if (any_no_face) {
      result.reason = "No face detected";
    } else {
      result.reason = "Authentication failed";
    }
  }

  recordAuthAttempt(std::string(username), result.success);
  return result;
}

bool AuthEngine::verifyUser(std::string_view username) {
  AuthResult res = verifyUserCore(
      username, [&](const std::string &id, const cv::Mat &frame, bool success,
                    float score, const std::string &msg) {
        // Logging & Saving Logic (from original verifyUser)
        if (msg == "Capture failed") {
          std::cout << "[AuthEngine] Camera " << id << " failed to capture."
                    << '\n';
          return;
        }

        std::string full_msg = "Camera " + id + " " + msg;
        if (score > 0)
          full_msg += " (Score: " + std::to_string(score) + ")";

        if (success) {
          log_info(full_msg);
          if (config.save_success) {
            std::string fn =
                (config.log_dir / ("success_" + id + "_" + std::string(username) + ".jpg")).string();
            if (!frame.empty())
              cv::imwrite(fn, frame);
          }
        } else {
          log_warn(full_msg);
          if (config.save_fail) {
            std::string prefix = (msg == "NO_FACE_DETECTED") ? "fail_"
                                 : (msg == "No embeddings found")
                                     ? "fail_missing_"
                                     : "fail_mismatched_";
            std::string fn =
                (config.log_dir / (prefix + id + "_" + std::string(username) + ".jpg")).string();
            if (!frame.empty()) {
              cv::imwrite(fn, frame);
              log_debug("Saved fail image to: " + fn);
            }
          }
        }
      });
  return res.success;
}

AuthResult AuthEngine::verifyUserWithDetails(std::string_view username) {
  // Used by CLI test command - now with logging enabled for debugging
  return verifyUserCore(username, [&](const std::string &id,
                                      const cv::Mat &frame, bool success,
                                      float score, const std::string &msg) {
    // Logging & Saving Logic (Shared with verifyUser)
    if (msg == "Capture failed") {
      return;
    }

    std::string full_msg = " [CLI] Camera " + id + " " + msg;
    if (score > 0)
      full_msg += " (Score: " + std::to_string(score) + ")";

    if (success) {
      log_info(full_msg);
      if (config.save_success) {
        std::string fn =
            (config.log_dir / ("success_test_" + id + "_" + std::string(username) + ".jpg")).string();
        if (!frame.empty())
          cv::imwrite(fn, frame);
      }
    } else {
      log_warn(full_msg);
      // Save fail images based on configuration
      if (config.save_fail) {
        std::string prefix = (msg == "NO_FACE_DETECTED") ? "fail_"
                             : (msg == "No embeddings found")
                                 ? "fail_missing_"
                                 : "fail_mismatched_";
        std::string fn =
            (config.log_dir / (prefix + "test_" + id + "_" + std::string(username) + ".jpg")).string();
        if (!frame.empty()) {
          cv::imwrite(fn, frame);
          log_debug("Saved test fail image to: " + fn);
        }
      }
    }
  });
}

// Atomic write: stage to temp file, fsync, then rename.
// mkostemp's O_EXCL prevents symlink/tempfile collisions.
static std::string writeJsonAtomic(const fs::path& final_path,
                                   const std::string& data) {
  // Ensure biometric directory remains 0700. Best-effort.
  if (const fs::path dir = final_path.parent_path(); !dir.empty()) {
    std::error_code perm_ec;
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, perm_ec);
  }

  std::string tmp_path = final_path.string() + ".tmp.XXXXXX";

  // Non-throwing cleanup. We're already in an error path; don't mask it.
  auto discard_tmp = [](const std::string &p) {
    std::error_code ec;
    fs::remove(p, ec);
  };

  {
    linuxcampam::FileDescriptor fd(mkostemp(tmp_path.data(), O_CLOEXEC));
    if (!fd.isValid()) {
      return "mkostemp failed: " + std::system_category().message(errno);
    }
    // Force 0600 (don't trust libc mkostemp defaults for biometrics).
    if (fchmod(fd.get(), linuxcampam::SECURE_FILE_MODE) < 0) {
      const int saved_errno = errno;
      discard_tmp(tmp_path);
      return "fchmod failed: " + std::system_category().message(saved_errno);
    }
    size_t written = 0;
    while (written < data.size()) {
      ssize_t n = write(fd.get(), data.data() + written, data.size() - written);
      if (n < 0) {
        if (errno == EINTR) continue;
        const int saved_errno = errno;
        discard_tmp(tmp_path);
        return "write failed: " + std::system_category().message(saved_errno);
      }
      written += static_cast<size_t>(n);
    }
    if (fsync(fd.get()) < 0) {
      const int saved_errno = errno;
      discard_tmp(tmp_path);
      return "fsync failed: " + std::system_category().message(saved_errno);
    }
  } // Must close fd before renaming
  std::error_code ec;
  fs::rename(tmp_path, final_path, ec);
  if (ec) {
    discard_tmp(tmp_path);  // don't leak the staged file on a failed rename
    return ec.message();
  }

  // Fsync parent dir so the rename survives a power loss. Best-effort.
  if (const fs::path dir = final_path.parent_path(); !dir.empty()) {
    linuxcampam::FileDescriptor dir_fd(
        open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (dir_fd.isValid())
      (void)fsync(dir_fd.get());
  }
  return "";
}

std::pair<bool, std::string>
AuthEngine::enrollUser(std::string_view username) {
  if (!ensureModelsLoaded())
    return {false, "Failed to load AI models."};
  if (!linuxcampam::isValidUsername(username)) {
    std::cerr << "[AuthEngine] Security Warn: Invalid username string: "
              << username << '\n';
    return {false, "Invalid username (security restriction)."};
  }

  // Load existing user file or create new
  fs::path user_file = config.users_dir / (std::string(username) + ".json");
  json j;
  if (auto j_opt = loadUserJsonSafe(user_file)) {
    j = *j_opt;
  } else {
    j["username"] = username;
    j["created"] = std::time(nullptr);
  }

  log_info("Enrolling user " + std::string(username) + " across " +
                                  std::to_string(active_cameras.size()) +
                                  " cameras.");

  for (auto &ac : active_cameras) {
    std::string id = ac.config.id;
    log_debug("Capturing from " + id + "...");

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
      log_error("Camera " + id + " failed. Enroll aborted.");
      return {false, "Camera " + id + " failed (empty frame)."};
    }

    // Align input size to 32 for YuNet stability

    // Use dynamic input size for enrollment
    // Detect and generate embedding
    std::vector<float> vec;
    cv::Mat aligned;
    int num_faces = generateEmbedding(frame, vec, aligned);

    if (num_faces != 1) {
      std::string err = "Found ";
      err += std::to_string(num_faces);
      err += " faces in ";
      err += id;
      err += ". Expecting exactly 1.";

      double brightness = calculateBrightness(frame);
      log_warn("Enroll failed: " + err +
                                      " | Frame: " +
                                      std::to_string(frame.cols) + "x" +
                                      std::to_string(frame.rows) +
                                      " | Brightness: " +
                                      std::to_string(static_cast<int>(brightness)) +
                                      " | threshold: " +
                                      std::to_string(config.detection_threshold));

      std::error_code dir_ec;
      fs::create_directories(config.log_dir, dir_ec);
      if (dir_ec)
        log_warn("Could not create log dir for debug frame: " + dir_ec.message());
      std::string fail_filename = (config.log_dir / "failed_enroll_").string();
      fail_filename += id;
      fail_filename += "_";
      fail_filename += std::string(username);
      fail_filename += ".jpg";
      if (cv::imwrite(fail_filename, frame)) {
        log_info("Debug frame saved: " + fail_filename +
                        " - inspect to verify camera output.");
      } else {
        log_warn("Could not save debug frame: " + fail_filename);
      }

      return {false, err};
    }

    // Store as pending embedding (will be finalized by setLabel)
    std::string pending_key = "_pending_" + ac.config.type;
    j[pending_key] = vec;
  }

  log_info("Saving pending enrollment...");
  std::error_code users_dir_ec;
  fs::create_directories(config.users_dir, users_dir_ec);
  if (users_dir_ec) {
    log_error("Failed to create user directory: " + users_dir_ec.message());
    return {false, "Failed to create user directory: " + users_dir_ec.message()};
  }
  if (auto err = writeJsonAtomic(user_file, j.dump(4)); !err.empty()) {
    log_error("Failed to write user file: " + err);
    return {false, "Write failed"};
  }
  return {true, "Success"};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool AuthEngine::setLabel(std::string_view username,
                          std::string_view label) {
  if (!linuxcampam::isValidUsername(username))
    return false;

  fs::path user_file = config.users_dir / (std::string(username) + ".json");
  auto j_opt = loadUserJsonSafe(user_file);
  if (!j_opt)
    return false;
  json j = *j_opt;

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
        auto &arr = j[emb_array_key];
        auto it = std::find_if(arr.begin(), arr.end(), [&](const json &entry) {
          return entry["label"] == label;
        });

        if (it != arr.end()) {
          (*it)["data"] = embedding_data;
          (*it)["created"] = std::time(nullptr);
          (*it)["model_version"] = getModelVersion(recognition_model_path);
        } else {
          log_warn("Max embeddings (" +
                          std::to_string(config.max_embeddings) +
                          ") reached for " + std::string(username));
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
    if (auto err = writeJsonAtomic(user_file, j.dump(4)); !err.empty()) {
      log_error("Failed to write user file: " + err);
      return false;
    }
    log_info("Set label '" + std::string(label) + "' for " + std::string(username));
  }
  return updated;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool AuthEngine::trainUser(std::string_view username,
                           std::string_view label, bool create_new) {
  if (!ensureModelsLoaded())
    return false;
  if (!linuxcampam::isValidUsername(username)) {
    log_warn("Security Warn: Invalid username string: " + std::string(username));
    return false;
  }
  fs::path user_file = config.users_dir / (std::string(username) + ".json");
  auto j_opt = loadUserJsonSafe(user_file);
  if (!j_opt)
    return false;
  json j = *j_opt;

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

    std::vector<float> new_vec;
    cv::Mat aligned;
    int num_faces = generateEmbedding(frame, new_vec, aligned);

    if (num_faces != 1) {
      log_warn("Train: Expected 1 face, found " + std::to_string(num_faces));
      continue;
    }

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
        log_warn("Max embeddings reached for " + std::string(username));
        return false;
      }
      json entry;
      entry["label"] = label.empty()
                           ? "trained_" + std::to_string(std::time(nullptr))
                           : label;
      entry["data"] = new_vec;
      entry["created"] = std::time(nullptr);
      j[emb_array_key].push_back(entry);
      log_info("Train: Added new embedding '" +
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
          // Reconstruct new_emb from vector
          cv::Mat new_emb(1, static_cast<int>(new_vec.size()), CV_32F,
                          new_vec.data());

          cv::Mat avg = old_emb + new_emb;
          cv::normalize(avg, avg);
          std::vector<float> avg_vec;
          avg.reshape(1, 1).copyTo(avg_vec);
          entry["data"] = avg_vec;
          entry["created"] = std::time(nullptr);
          found = true;
          log_info("Train: Refined embedding '" + std::string(label) + "'");
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
        log_info("Train: Created new embedding '" + std::string(label) + "'");
        updated_any = true;
      }
    }
  }

  if (updated_any) {
    if (auto err = writeJsonAtomic(user_file, j.dump(4)); !err.empty()) {
      log_error("Failed to write user file: " + err);
      return false;
    }
  }
  return updated_any;
}

std::vector<std::string>
AuthEngine::listEmbeddings(std::string_view username) {
  std::set<std::string> unique_labels;
  if (!linuxcampam::isValidUsername(username))
    return {};

  fs::path user_file = config.users_dir / (std::string(username) + ".json");
  auto j_opt = loadUserJsonSafe(user_file);
  if (!j_opt)
    return {};
  json j = *j_opt;

  for (const auto &ac : active_cameras) {
    std::string emb_array_key = "embeddings_" + ac.config.type;
    if (j.contains(emb_array_key) && j[emb_array_key].is_array()) {
      for (const auto &entry : j[emb_array_key]) {
        if (entry.contains("label")) {
          unique_labels.insert(entry["label"].get<std::string>());
        }
      }
    }
    // Check legacy format
    std::string emb_key = "embedding_" + ac.config.type;
    if (j.contains(emb_key)) {
      unique_labels.insert("default (legacy)");
    }
  }
  return {unique_labels.begin(), unique_labels.end()};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool AuthEngine::removeEmbedding(std::string_view username,
                                 std::string_view label) {
  if (!linuxcampam::isValidUsername(username))
    return false;

  fs::path user_file = config.users_dir / (std::string(username) + ".json");
  auto j_opt = loadUserJsonSafe(user_file);
  if (!j_opt)
    return false;
  json j = *j_opt;

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
    if (auto err = writeJsonAtomic(user_file, j.dump(4)); !err.empty()) {
      log_error("Failed to write user file: " + err);
      return false;
    }
    log_info("Removed embedding '" + std::string(label) + "' for " + std::string(username));
  }
  return removed;
}

bool AuthEngine::testCameraAndAuth() {
  if (!ensureModelsLoaded())
    return false;
  bool any_ok = false;
  log_info("Testing " + std::to_string(active_cameras.size()) + " cameras.");

  for (auto &ac : active_cameras) {
    std::string id = ac.config.id;
    log_info("Testing Camera " + id + "...");
    cv::Mat frame = captureFrame(ac.cam.get());
    if (!frame.empty()) {
      cv::Mat processed = frame;

      log_info("Frame Props: " + std::to_string(processed.cols) + "x" +
                      std::to_string(processed.rows) +
                      " Type=" + std::to_string(processed.type()) +
                      " Channels=" + std::to_string(processed.channels()));

      if (processed.channels() == 1) {
        log_info("Converting GRAY to BGR for detection");
        cv::cvtColor(processed, processed, cv::COLOR_GRAY2BGR);
      } else if (processed.channels() == 4) {
        log_info("Converting BGRA to BGR for detection");
        cv::cvtColor(processed, processed, cv::COLOR_BGRA2BGR);
      }

      try {
        log_info("Running detector on frame...");
        cv::Mat faces;
        detector->detect(processed, faces);
        any_ok = true;

        // Visualize result
        if (faces.rows > 0) {
          log_info("Detected " + std::to_string(faces.rows) +
                                          " faces on Camera " + id);
        } else {
          log_info("No faces detected on Camera " + id);
        }
      } catch (const cv::Exception &e) {
        log_error("OpenCV Exception handling TEST_AUTH: " +
                                         std::string(e.what()));
      } catch (const std::exception &e) {
        log_error("Exception handling TEST_AUTH: " + std::string(e.what()));
      } catch (...) {
        log_error("Unknown Exception handling TEST_AUTH");
      }
    } else {
      log_error("  -> Capture Failed.");
    }
  }

  return any_ok;
}

bool AuthEngine::isUserLockedOut(std::string_view username) {
  if (config.lockout_attempts <= 0)
    return false;

  std::scoped_lock lock(lockout_mutex_);
  if (auto it = lockout_map_.find(std::string(username)); it != lockout_map_.end()) {
    return std::chrono::steady_clock::now() < it->second.lockout_until;
  }
  return false;
}

void AuthEngine::recordAuthAttempt(std::string_view username, bool success) {
  if (config.lockout_attempts <= 0)
    return;

  std::scoped_lock lock(lockout_mutex_);
  auto &state = lockout_map_[std::string(username)];

  if (success) {
    state.failed_attempts = 0;
    state.lockout_until = {};
  } else {
    state.failed_attempts++;
    if (state.failed_attempts >= config.lockout_attempts) {
      state.lockout_until = std::chrono::steady_clock::now() +
                            std::chrono::seconds(config.lockout_duration_sec);
      log_warn(std::string(username) + " locked out");
    }
  }
}

std::string AuthEngine::getActiveProvider() const {
  for (std::string_view prov : config.provider_priority) {
    if (prov == "OpenCL") {
      if (cv::ocl::haveOpenCL()) {
        return "OpenCL";
      }
    } else if (prov == "CPU") {
      return "CPU";
    }
  }
  return "CPU (Default)";
}

std::string AuthEngine::getConfigString() const {
  std::string config_output = config.toString();

  std::string provider = getActiveProvider();
  config_output +=
      "  Active Provider: " + (provider.empty() ? "None" : provider) + "\n\n";

  return config_output;
}
