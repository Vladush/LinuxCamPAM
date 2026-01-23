#pragma once

#include "config.hpp"
#include "icamera.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
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

  // Allows to swap out the camera implementation (e.g., using a mock for
  // testing).
  using CameraFactory = std::function<std::unique_ptr<ICamera>(
      const Configuration::CameraDefinition &)>;
  void setCameraFactory(CameraFactory factory);

private:
  Configuration config;
  CameraFactory camera_factory_;

  cv::Ptr<cv::FaceDetectorYN> detector;
  cv::Ptr<cv::FaceRecognizerSF> recognizer;

  fs::path detection_model_path;
  fs::path recognition_model_path;

  // Helper struct to hold a running camera and its config
  struct ActiveCamera {
    std::unique_ptr<ICamera> cam;
    Configuration::CameraDefinition config;
  };

  std::vector<ActiveCamera> active_cameras;

  // Internal helper to capture from a specific camera instance
  cv::Mat captureFrame(ICamera *cam);

  // Helper to initialize active cameras from config
  void initializeActiveCameras();

  using PerCameraCallback =
      std::function<void(const std::string &cam_id, const cv::Mat &frame,
                         bool success, float score, const std::string &msg)>;

  // Core verification logic (PAM + CLI)
  [[nodiscard]] AuthResult
  verifyUserCore(const std::string &username,
                 const PerCameraCallback &callback = nullptr);

  // Helper to match a face in a frame against a stored embedding
  // Returns score (0.0 - 1.0)
  [[nodiscard]] float matchFace(const cv::Mat &frame, const cv::Mat &stored_emb,
                                cv::Mat &out_face);

  // Helper to calculate brightness
  [[nodiscard]] double calculateBrightness(const cv::Mat &frame);
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
  [[nodiscard]] bool isUserLockedOut(const std::string &username);
  void recordAuthAttempt(const std::string &username, bool success);
};
