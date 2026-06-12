#pragma once

#include "config.hpp"
#include "icamera.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <opencv2/dnn.hpp>
#include <opencv2/opencv.hpp>
#include <string>
#include <unordered_map>
#include <utility>
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
  [[nodiscard]] bool verifyUser(std::string_view username);
  [[nodiscard]] AuthResult verifyUserWithDetails(std::string_view username);
  [[nodiscard]] std::pair<bool, std::string>
  enrollUser(std::string_view username);
  // NOLINTBEGIN(bugprone-easily-swappable-parameters)
  // Risk of swapping username and label is low since they are strictly validated
  // by the IPC protocol parser in main.cpp. Strong types are overkill here.
  [[nodiscard]] bool setLabel(std::string_view username,
                              std::string_view label);
  [[nodiscard]] bool trainUser(std::string_view username,
                               std::string_view label = "default",
                               bool create_new = false);
  // NOLINTEND(bugprone-easily-swappable-parameters)
  [[nodiscard]] bool testCameraAndAuth();
  [[nodiscard]] bool performMaintenance();

  // Multi-embedding management
  [[nodiscard]] std::vector<std::string>
  listEmbeddings(std::string_view username);
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] bool removeEmbedding(std::string_view username,
                                     std::string_view label);

  // Config visibility
  [[nodiscard]] std::string getConfigString() const;
  [[nodiscard]] const Configuration &getConfig() const { return config; }
  [[nodiscard]] std::string getActiveProvider() const;

  // Allows to swap out the camera implementation (e.g., using a mock for
  // testing).
  using CameraFactory = std::function<std::unique_ptr<ICamera>(
      const Configuration::CameraDefinition &)>;
  void setCameraFactory(CameraFactory factory);

  // Expose lockout state for tests and daemon maintenance.
  [[nodiscard]] bool isUserLockedOut(std::string_view username);
  void recordAuthAttempt(std::string_view username, bool success);

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
  static cv::Mat captureFrame(ICamera *cam);

  // Helper to generate embedding from a frame.
  // Returns number of faces found. Populates out_embedding and out_aligned_face
  // using the largest face.
  int generateEmbedding(const cv::Mat &frame, std::vector<float> &out_embedding,
                        cv::Mat &out_aligned_face);

  // Helper to initialize active cameras from config
  void initializeActiveCameras();

  using PerCameraCallback =
      std::function<void(const std::string &cam_id, const cv::Mat &frame,
                         bool success, float score, const std::string &msg)>;

  // Core verification logic (PAM + CLI)
  [[nodiscard]] AuthResult
  verifyUserCore(std::string_view username,
                 const PerCameraCallback &callback = nullptr);

  // Helper to match a face in a frame against a stored embedding
  // Returns score (0.0 - 1.0)
  [[nodiscard]] float matchFace(const cv::Mat &frame, const cv::Mat &stored_emb,
                                cv::Mat &out_face);

  // Helper to calculate brightness
  [[nodiscard]] static double calculateBrightness(const cv::Mat &frame);
  void fallbackToCPU();

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
};
