#pragma once

#include "constants.hpp"

#include <opencv2/opencv.hpp>

class ICamera {
public:
  virtual ~ICamera() = default;

  // Rule of 5
  ICamera() = default;
  ICamera(const ICamera &) = delete;
  ICamera &operator=(const ICamera &) = delete;
  ICamera(ICamera &&) = delete;
  ICamera &operator=(ICamera &&) = delete;

  virtual void triggerIrEmitter() = 0;

  // Standard capture (for verification - fast)
  virtual cv::Mat capture() = 0;

  // Enhanced capture methods (for enrollment - quality)
  virtual cv::Mat
  captureAveraged(int num_frames = linuxcampam::CAMERA_AVERAGE_FRAMES) = 0;
  virtual cv::Mat captureHDR() = 0; // Multi-exposure

  // Capability detection
  [[nodiscard]] virtual bool supportsManualExposure() const = 0;
};
