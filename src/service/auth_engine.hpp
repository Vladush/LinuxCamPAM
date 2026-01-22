#pragma once

#include "constants.hpp"
#include "icamera.hpp"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <opencv2/dnn.hpp>
#include <opencv2/opencv.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

// Constants moved to AuthEngine class to avoid anonymous namespace in header

// Helper: Cosine similarity between two feature vectors.
// NOTE: With extreme values (>1e30), overflow to inf/inf produces NaN.
// This is acceptable since real embeddings are normalized to [-1, 1] range.
// If hardening is needed, use double precision or pre-normalize inputs.
inline float cosine_similarity(const cv::Mat &a, const cv::Mat &b) {
  return static_cast<float>(a.dot(b) / (cv::norm(a) * cv::norm(b)));
}

// Detailed auth result for diagnostics
struct AuthResult {
  bool success = false;
  std::string reason; // Empty on success, or: "User not enrolled", "No face
                      // detected", etc.
  float best_score = 0.0f;
};

class AuthEngine {
public:
  AuthEngine();
  ~AuthEngine();

  // Rule of 5: Resource management (OpenCV pointers, mutexes) requires explicit
  // handling
  AuthEngine(const AuthEngine &) = delete;
  AuthEngine &operator=(const AuthEngine &) = delete;
  AuthEngine(AuthEngine &&) = delete;
  AuthEngine &operator=(AuthEngine &&) = delete;

  [[nodiscard]] bool init(const fs::path &config_path);

  // Operations
  [[nodiscard]] bool verifyUser(const std::string &username);
  [[nodiscard]] AuthResult verifyUserWithDetails(const std::string &username);
  [[nodiscard]] std::pair<bool, std::string>
  enrollUser(const std::string &username);
  [[nodiscard]] bool setLabel(const std::string &username,
                              const std::string &label);
  [[nodiscard]] bool trainUser(const std::string &username,
                               const std::string &label = "default",
                               bool create_new = false);
  [[nodiscard]] bool testCameraAndAuth();
  [[nodiscard]] bool performMaintenance();

  // Multi-embedding management
  [[nodiscard]] std::vector<std::string>
  listEmbeddings(const std::string &username);
  [[nodiscard]] bool removeEmbedding(const std::string &username,
                                     const std::string &label);

  // Config visibility
  [[nodiscard]] std::string getConfigString() const;

private:
  static constexpr float DEFAULT_THRESHOLD = 0.363f;
  static constexpr float DEFAULT_DETECTION_THRESHOLD = 0.9f;
  static constexpr int DEFAULT_TIMEOUT_MS = 3000;
  static constexpr int DEFAULT_MAX_EMBEDDINGS = 5;
  static constexpr int DEFAULT_ENROLL_AVG_FRAMES = 5;
  static constexpr int DEFAULT_VERIFY_AVG_FRAMES = 3;
  static constexpr int DEFAULT_LOCKOUT_ATTEMPTS = 5;
  static constexpr int DEFAULT_LOCKOUT_DURATION_SEC = 300;
  static constexpr int DEFAULT_GPU_THROTTLE_MS = 20;

  enum class AuthPolicy {
    STRICT_ALL,  // All cameras must match
    LENIENT_ANY, // At least one camera must match
    ADAPTIVE     // Legacy logic: IR mandatory, RGB conditional
  };

  struct CameraDefinition {
    std::string id;
    std::string path;
    std::string type; // "ir", "rgb"
    int min_brightness = 0;
    bool mandatory = false; // For ADAPTIVE policy

    // Per-camera capture settings (override global if set)
    std::string enroll_hdr = ""; // "", "auto", "on", "off" - empty = use global
    std::string enroll_averaging = ""; // "", "on", "off" - empty = use global
    int enroll_average_frames = 0;     // 0 = use global
  };

  // Helper struct to hold a running camera and its config
  struct ActiveCamera {
    std::unique_ptr<ICamera> cam;
    CameraDefinition config;
  };

  struct Config {
    float threshold = DEFAULT_THRESHOLD;
    float detection_threshold = DEFAULT_DETECTION_THRESHOLD;
    int timeout_ms = DEFAULT_TIMEOUT_MS;
    int max_embeddings = DEFAULT_MAX_EMBEDDINGS; // 0 = unlimited

    AuthPolicy policy = AuthPolicy::ADAPTIVE;
    std::vector<CameraDefinition> camera_defs;

    bool save_success = false;
    bool save_fail = false;
    std::string log_dir = "/var/log/linuxcampam/";
    std::vector<std::string> provider_priority;
    int model_keep_alive_sec = 0; // 0 = Always loaded

    // Capture settings
    std::string enroll_hdr = "auto"; // auto | on | off
    bool enroll_averaging = true;
    int enroll_average_frames = DEFAULT_ENROLL_AVG_FRAMES;
    bool verify_averaging = false;
    int verify_average_frames = DEFAULT_VERIFY_AVG_FRAMES;

    // Paths
    // Paths
    fs::path users_dir = linuxcampam::USERS_DIR;
    fs::path models_dir = linuxcampam::MODELS_DIR;
    fs::path ir_emitter_path = linuxcampam::IR_EMITTER_PATH;

    // Security / Rate Limiting
    int lockout_attempts =
        DEFAULT_LOCKOUT_ATTEMPTS; // Lock after N failures. 0 = disabled.
    int lockout_duration_sec =
        DEFAULT_LOCKOUT_DURATION_SEC; // Lockout duration (5 min default)

    // GPU sync options
    bool gpu_flush = false;
    int gpu_throttle_ms = DEFAULT_GPU_THROTTLE_MS;
  } config;

  cv::Ptr<cv::FaceDetectorYN> detector;
  cv::Ptr<cv::FaceRecognizerSF> recognizer;

  fs::path detection_model_path;
  fs::path recognition_model_path;

  std::vector<ActiveCamera> active_cameras;

  // Internal helper to capture from a specific camera instance
  cv::Mat captureFrame(ICamera *cam);

  // Helper to match a face in a frame against a stored embedding
  // Returns score (0.0 - 1.0)
  float matchFace(const cv::Mat &frame, const cv::Mat &stored_emb,
                  cv::Mat &out_face);

  // Helper to calculate brightness
  double calculateBrightness(const cv::Mat &frame);
  void fallbackToCPU();

  // Security
  [[nodiscard]] bool isValidUsername(std::string_view username);

  // Dynamic Loading
  [[nodiscard]] bool ensureModelsLoaded();
  [[nodiscard]] bool loadModels();
  void unloadModels();

  std::chrono::steady_clock::time_point last_activity_;

  // Lockout state
  struct LockoutState {
    int failed_attempts = 0;
    std::chrono::steady_clock::time_point lockout_until{};
  };
  std::unordered_map<std::string, LockoutState> lockout_map_;
  mutable std::mutex lockout_mutex_;
  bool isUserLockedOut(const std::string &username);
  void recordAuthAttempt(const std::string &username, bool success);
};
