#pragma once

#include "camera.hpp"
#include "constants.hpp"

#include <chrono>
#include <opencv2/dnn.hpp>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

// Helper: Cosine similarity between two feature vectors.
// NOTE: With extreme values (>1e30), overflow to inf/inf produces NaN.
// This is acceptable since real embeddings are normalized to [-1, 1] range.
// If hardening is needed, use double precision or pre-normalize inputs.
inline float cosine_similarity(const cv::Mat &a, const cv::Mat &b) {
  return a.dot(b) / (cv::norm(a) * cv::norm(b));
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

  [[nodiscard]] bool init(const std::string &config_path);

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

private:
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
    std::unique_ptr<Camera> cam;
    CameraDefinition config;
  };

  struct Config {
    float threshold = 0.363f;
    float detection_threshold = 0.9f;
    int timeout_ms = 3000;
    int max_embeddings = 5; // 0 = unlimited

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
    int enroll_average_frames = 5;
    bool verify_averaging = false;
    int verify_average_frames = 3;

    // Paths
    std::string users_dir = linuxcampam::USERS_DIR;
    std::string models_dir = linuxcampam::MODELS_DIR;
    std::string ir_emitter_path = linuxcampam::IR_EMITTER_PATH;
  } config;

  cv::Ptr<cv::FaceDetectorYN> detector;
  cv::Ptr<cv::FaceRecognizerSF> recognizer;

  std::string detection_model_path;
  std::string recognition_model_path;

  std::vector<ActiveCamera> active_cameras;

  // Internal helper to capture from a specific camera instance
  cv::Mat captureFrame(Camera *cam);

  // Helper to match a face in a frame against a stored embedding
  // Returns score (0.0 - 1.0)
  float matchFace(const cv::Mat &frame, const cv::Mat &stored_emb,
                  cv::Mat &out_face);

  // Helper to calculate brightness
  double calculateBrightness(const cv::Mat &frame);
  void fallbackToCPU();

  // Security
  [[nodiscard]] bool isValidUsername(const std::string &username);

  // Dynamic Loading
  [[nodiscard]] bool ensureModelsLoaded();
  [[nodiscard]] bool loadModels();
  void unloadModels();

  std::chrono::steady_clock::time_point last_activity_;
};
