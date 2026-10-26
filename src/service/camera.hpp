#pragma once

#include "constants.hpp"
#include "icamera.hpp"

#include <filesystem>
#include <opencv2/opencv.hpp>
#include <string>

namespace fs = std::filesystem;

class Camera : public ICamera {
public:
  explicit Camera(std::string_view device_path, bool is_ir = false,
                  const fs::path &ir_cmd_path = "",
                  bool ambient_subtraction = false);
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

protected:
  std::string raw_device_path_;
  std::string device_path;
  fs::path ir_emitter_path_;
  int device_id = -1;
  bool is_ir_camera = false;
  bool supports_manual_exposure_ = false;
  bool ambient_subtraction_ = false;

  virtual bool detectExposureSupport();
  virtual bool openAndWarmup(cv::VideoCapture &temp_cap);
  virtual cv::Mat captureSingleOnSession(cv::VideoCapture &temp_cap);
  virtual cv::Mat captureAverageOnSession(cv::VideoCapture &temp_cap, int num_frames);
};
