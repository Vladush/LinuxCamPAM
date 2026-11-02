#pragma once

#include "constants.hpp"
#include "icamera.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <opencv2/opencv.hpp>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

class ICameraSession {
public:
  virtual ~ICameraSession() = default;
  virtual bool open(std::string_view path, int device_id) = 0;
  [[nodiscard]] virtual bool isOpened() const = 0;
  [[nodiscard]] virtual bool detectExposureSupport(std::string_view path) = 0;
  virtual bool read(cv::Mat &frame) = 0;
  virtual bool grab() = 0;
  [[nodiscard]] virtual double getProperty(int prop_id) const = 0;
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  virtual bool setProperty(int prop_id, double value) = 0;
  virtual void sleepMs(int ms) = 0;
};

class Camera : public ICamera {
public:
  using SessionFactory = std::function<std::unique_ptr<ICameraSession>()>;

  explicit Camera(std::string_view device_path, bool is_ir = false,
                  const fs::path &ir_cmd_path = "",
                  bool ambient_subtraction = false,
                  SessionFactory session_factory = nullptr);
  ~Camera() override;

  // Delete copy/move to enforce unique ownership (Rule of 5)
  Camera(const Camera &) = delete;
  Camera &operator=(const Camera &) = delete;
  Camera(Camera &&) = delete;
  Camera &operator=(Camera &&) = delete;

  void triggerIrEmitter() override;

  // Standard capture (for verification - fast)
  [[nodiscard]] cv::Mat capture() override;

  // Enhanced capture methods (for enrollment - quality)
  [[nodiscard]] cv::Mat
  captureAveraged(int num_frames = linuxcampam::CAMERA_AVERAGE_FRAMES) override;
  [[nodiscard]] cv::Mat
  captureHDR() override; // Multi-exposure, requires manual exposure support
  [[nodiscard]] cv::Mat
  captureEnrollment(std::string_view hdr_mode, bool use_averaging, int avg_frames) override;

  // Capability detection
  bool supportsManualExposure() const override {
    return supports_manual_exposure_;
  }

  void setAmbientSubtraction(bool enabled) { ambient_subtraction_ = enabled; }
  [[nodiscard]] bool ambientSubtraction() const { return ambient_subtraction_; }

  void setSessionFactory(SessionFactory factory) {
    session_factory_ = std::move(factory);
  }

private:
  std::string raw_device_path_;
  std::string device_path;
  fs::path ir_emitter_path_;
  int device_id = -1;
  bool is_ir_camera = false;
  bool supports_manual_exposure_ = false;
  bool ambient_subtraction_ = false;
  SessionFactory session_factory_;

  [[nodiscard]] std::unique_ptr<ICameraSession> createSession() const;
  bool openAndWarmup(ICameraSession &session);
  cv::Mat captureSingleOnSession(ICameraSession &session);
  cv::Mat captureAverageOnSession(ICameraSession &session, int num_frames);
};
