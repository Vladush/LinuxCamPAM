#pragma once

#include "camera_interface.hpp"
#include "constants.hpp"

#include <opencv2/opencv.hpp>
#include <string>

class Camera : public ICamera {
public:
  explicit Camera(const std::string &device_path, bool is_ir = false,
                  const std::string &ir_cmd_path = "");
  ~Camera();

  // Delete copy/move to enforce unique ownership (Rule of 5)
  Camera(const Camera &) = delete;
  Camera &operator=(const Camera &) = delete;
  Camera(Camera &&) = delete;
  Camera &operator=(Camera &&) = delete;

  void triggerIrEmitter() override;

  // Standard capture (for verification - fast)
  cv::Mat capture() override;

  // Enhanced capture methods (for enrollment - quality)
  cv::Mat
  captureAveraged(int num_frames = linuxcampam::CAMERA_AVERAGE_FRAMES) override;
  cv::Mat
  captureHDR() override; // Multi-exposure, requires manual exposure support

  // Capability detection
  bool supportsManualExposure() const override {
    return supports_manual_exposure_;
  }

private:
  std::string device_path;
  std::string ir_emitter_path_;
  int device_id = 0;
  bool is_ir_camera = false;
  bool supports_manual_exposure_ = false;
  cv::VideoCapture cap;

  bool detectExposureSupport();
  bool openAndWarmup(cv::VideoCapture &cap);
};
