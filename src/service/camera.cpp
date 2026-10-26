#include "camera.hpp"
#include "camera_frames.hpp"
#include "constants.hpp"
#include "logger.hpp"
#include "utils.hpp"
#include "../common/device_validator.hpp"
#include <array>
#include <fcntl.h>
#include <filesystem>
#include <linux/videodev2.h>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/photo.hpp>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

// Required for posix_spawn environment inheritance
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
extern char **environ;

namespace {
using linuxcampam::FileDescriptor;
}

void Camera::triggerIrEmitter() {
  log_info("[Camera] Triggering IR emitter");

  pid_t pid = 0;
  std::string cmd = "run";
  std::vector<char *> args;

  // Prepare argv for posix_spawn (requires mutable char* array)
  // We copy strings into mutable buffers, then create pointers to them.
  std::string path_str = ir_emitter_path_.string();
  std::vector<char> path_buf(path_str.begin(), path_str.end());
  path_buf.push_back('\0');

  std::vector<char> cmd_buf(cmd.begin(), cmd.end());
  cmd_buf.push_back('\0');

  args.push_back(path_buf.data());
  args.push_back(cmd_buf.data());
  args.push_back(nullptr);

  int status = posix_spawn(&pid, ir_emitter_path_.c_str(), nullptr, nullptr,
                           args.data(), environ);

  if (status == 0) {
    constexpr int IR_TIMEOUT_MS = 5000;
    int wait_status = 0;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    FileDescriptor pidfd(static_cast<int>(syscall(SYS_pidfd_open, pid, 0)));
    if (pidfd.isValid()) {
      // poll() returns immediately when the process exits
      struct pollfd pfd = {};
      pfd.fd     = pidfd.get();
      pfd.events = POLLIN;
      // Shrink timeout on EINTR retries to strictly enforce the deadline.
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(IR_TIMEOUT_MS);
      int ret = poll(&pfd, 1, IR_TIMEOUT_MS);
      while (ret < 0 && errno == EINTR) {
        const int remaining = linuxcampam::poll_remaining_ms(
            deadline, std::chrono::steady_clock::now());
        if (remaining == 0) {  // deadline reached; treat as timeout
          ret = 0;
          break;
        }
        ret = poll(&pfd, 1, remaining);
      }
      if (ret == 0) {
        log_error("[Camera] IR emitter timed out; sending SIGKILL.");
        kill(pid, SIGKILL);
      }
      while (waitpid(pid, &wait_status, 0) == -1 && errno == EINTR) {}
    } else {
      // Fallback to sleep-poll if pidfd_open fails
      log_warn("[Camera] pidfd_open failed; falling back to sleep-poll.");
      constexpr int POLL_INTERVAL_MS = 100;
      bool exited = false;
      for (int i = 0; i < IR_TIMEOUT_MS / POLL_INTERVAL_MS; ++i) {
        const pid_t w = waitpid(pid, &wait_status, WNOHANG);
        if (w == pid) { exited = true; break; }
        if (w == -1 && errno != EINTR) { wait_status = -1; exited = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
      }
      if (!exited) {
        log_error("[Camera] IR emitter timed out; sending SIGKILL.");
        kill(pid, SIGKILL);
        while (waitpid(pid, &wait_status, 0) == -1 && errno == EINTR) {}
      }
    }

    if (wait_status != -1 && WIFEXITED(wait_status)) {
      log_info("[Camera] IR emitter exited with code: " +
               std::to_string(WEXITSTATUS(wait_status)));
    } else {
      log_error("[Camera] IR emitter terminated abnormally.");
    }
  } else {
    std::ostringstream oss;
    oss << "[Camera] posix_spawn failed for " << ir_emitter_path_ << ": "
        << std::system_category().message(status) << " (" << status << ")";
    log_error(oss.str());
  }
}

bool Camera::detectExposureSupport() {
  if (device_id < 0) {
    return false;
  }
  FileDescriptor fd_wrapper(open(device_path.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK));
  if (!fd_wrapper.isValid())
    return false;

  struct stat st {};
  if (fstat(fd_wrapper.get(), &st) != 0 || !S_ISCHR(st.st_mode)) {
    return false;
  }

  struct v4l2_queryctrl queryctrl = {};
  queryctrl.id = V4L2_CID_EXPOSURE_ABSOLUTE;
  bool supported =
      (ioctl(fd_wrapper.get(), VIDIOC_QUERYCTRL, &queryctrl) == 0) &&
      !(queryctrl.flags & V4L2_CTRL_FLAG_DISABLED);
  return supported;
}

Camera::Camera(std::string_view device_path, bool is_ir,
               const fs::path &ir_cmd_path, bool ambient_subtraction)
    : raw_device_path_(device_path),
      device_path(device_path),
      is_ir_camera(is_ir),
      ambient_subtraction_(ambient_subtraction) {
  if (ir_cmd_path.empty()) {
    ir_emitter_path_ = linuxcampam::IR_EMITTER_PATH;
  } else {
    ir_emitter_path_ = ir_cmd_path;
  }
  cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);

  // Perform lexical path extraction without opening hardware in constructor
  auto resolved = linuxcampam::resolveCameraPath(raw_device_path_, /*require_char_device=*/false);
  if (resolved) {
    device_id = resolved->device_id;
    this->device_path = resolved->canonical_path;
  } else {
    device_id = -1;
    log_error("[Camera] Invalid camera device path: " + raw_device_path_);
  }
}

Camera::~Camera() {}

bool Camera::openAndWarmup(cv::VideoCapture &temp_cap) {
  // Open camera FIRST, then trigger IR emitter
  // IR state resets when camera device is released, so we must keep it open
  for (int attempt = 0; attempt < linuxcampam::CAPTURE_RETRY_ATTEMPTS; ++attempt) {
    auto resolved = linuxcampam::resolveCameraPath(raw_device_path_, /*require_char_device=*/true);
    if (resolved) {
      device_id = resolved->device_id;
      this->device_path = resolved->canonical_path;
      if (temp_cap.open(device_id, cv::CAP_V4L2)) {
        break;
      }
    }
    if (attempt + 1 < linuxcampam::CAPTURE_RETRY_ATTEMPTS) {
      log_warn("[Camera] Device unavailable or busy. Retrying (" + std::to_string(attempt + 1) +
               "/" + std::to_string(linuxcampam::CAPTURE_RETRY_ATTEMPTS) + ")...");
      std::this_thread::sleep_for(
          std::chrono::seconds(linuxcampam::CAPTURE_RETRY_DELAY_S));
    }
  }

  if (!temp_cap.isOpened())
    return false;

  // Query hardware exposure support once device is opened and confirmed S_ISCHR
  supports_manual_exposure_ = detectExposureSupport();

  // Now trigger IR emitter while camera is open
  if (is_ir_camera) {
    triggerIrEmitter();
    std::this_thread::sleep_for(
        std::chrono::milliseconds(linuxcampam::IR_TRIGGER_DELAY_MS));
  }

  return true;
}

cv::Mat Camera::capture() {
  cv::VideoCapture temp_cap;
  if (!openAndWarmup(temp_cap)) {
    log_error("[Camera] Failed to open " + device_path);
    return cv::Mat();
  }

  cv::Mat frame;
  // Discard initial frames for auto-exposure settling
  for (int i = 0; i < linuxcampam::CAMERA_WARMUP_FRAMES; i++)
    temp_cap.read(frame); // Read and discard

  std::this_thread::sleep_for(
      std::chrono::milliseconds(linuxcampam::CAMERA_WARMUP_DELAY_MS));
  for (int i = 0; i < linuxcampam::CAMERA_DRAIN_GRAB_COUNT; ++i) {
    temp_cap.grab();
  }

  if (!is_ir_camera) {
    temp_cap.read(frame);
    return frame.empty() ? cv::Mat() : frame.clone();
  }

  // IR camera: select the best lit frame, avoiding alternating dark frames and overexposure
  auto reader = [&temp_cap](cv::Mat &out) {
    return temp_cap.read(out);
  };
  cv::Mat best = linuxcampam::selectBestLitFrame(
      reader, linuxcampam::kDefaultMaxCandidateReads, /*ambient_subtraction=*/false);
  if (best.empty()) {
    log_warn("[Camera] No illuminated frame detected for IR camera " + device_path);
    return cv::Mat();
  }
  return best;
}

cv::Mat Camera::captureAveraged(int num_frames) {
  if (num_frames <= 1) {
    return capture();
  }

  cv::VideoCapture temp_cap;
  if (!openAndWarmup(temp_cap)) {
    log_error("[Camera] Failed to open for averaging " + device_path);
    return cv::Mat();
  }

  // Warmup drain
  cv::Mat frame;
  for (int i = 0; i < linuxcampam::CAMERA_WARMUP_FRAMES; i++)
    temp_cap.read(frame);

  std::this_thread::sleep_for(
      std::chrono::milliseconds(linuxcampam::CAMERA_WARMUP_DELAY_MS));
  for (int i = 0; i < linuxcampam::CAMERA_DRAIN_GRAB_COUNT; ++i) {
    temp_cap.grab();
  }

  auto reader = [&temp_cap](cv::Mat &out) {
    return temp_cap.read(out);
  };

  std::vector<cv::Mat> frames_to_average;
  if (is_ir_camera) {
    frames_to_average = linuxcampam::collectLitFrames(
        reader, num_frames, -1, /*ambient_subtraction=*/false);
  } else {
    frames_to_average.reserve(static_cast<size_t>(num_frames));
    for (int i = 0; i < num_frames; i++) {
      if (temp_cap.read(frame) && !frame.empty()) {
        frames_to_average.push_back(frame.clone());
      }
    }
  }

  if (frames_to_average.size() < static_cast<size_t>(num_frames)) {
    log_warn("[Camera] Failed to collect sufficient frames for averaging (" +
             std::to_string(frames_to_average.size()) + "/" +
             std::to_string(num_frames) + ") on " + device_path);
    return cv::Mat();
  }

  log_info("[Camera] Averaged " + std::to_string(frames_to_average.size()) +
           " frames on " + device_path);
  return linuxcampam::averageFrames(frames_to_average);
}

cv::Mat Camera::captureEnrollment(std::string_view hdr_mode, bool use_averaging,
                                  int avg_frames) {
  cv::VideoCapture temp_cap;
  if (!openAndWarmup(temp_cap)) {
    log_error("[Camera] Failed to open for enrollment capture: " + device_path);
    return cv::Mat();
  }

  // Warmup drain
  cv::Mat frame;
  for (int i = 0; i < linuxcampam::CAMERA_WARMUP_FRAMES; i++) {
    temp_cap.read(frame);
  }

  std::this_thread::sleep_for(
      std::chrono::milliseconds(linuxcampam::CAMERA_WARMUP_DELAY_MS));
  for (int i = 0; i < linuxcampam::CAMERA_DRAIN_GRAB_COUNT; ++i) {
    temp_cap.grab();
  }

  // Capability has been probed on this active session in openAndWarmup()
  bool is_hdr_explicit = (hdr_mode == "on" || hdr_mode == "true");
  bool is_hdr_auto = (hdr_mode == "auto");

  bool do_hdr = false;
  if (is_hdr_explicit) {
    if (supports_manual_exposure_) {
      do_hdr = true;
    } else {
      if (use_averaging && avg_frames > 1) {
        log_warn("[Camera] Explicit HDR requested ('on'), but manual exposure unsupported on " +
                 device_path + "; falling back to averaging (" + std::to_string(avg_frames) + " frames)");
      } else {
        log_warn("[Camera] Explicit HDR requested ('on'), but manual exposure unsupported on " +
                 device_path + "; falling back to standard single capture");
      }
    }
  } else if (is_hdr_auto) {
    if (supports_manual_exposure_) {
      do_hdr = true;
    } else {
      if (use_averaging && avg_frames > 1) {
        log_info("[Camera] HDR auto: manual exposure unsupported on " + device_path +
                 "; using configured averaging (" + std::to_string(avg_frames) + " frames)");
      } else {
        log_info("[Camera] HDR auto: manual exposure unsupported on " + device_path +
                 "; using standard single capture");
      }
    }
  }

  if (do_hdr) {
    // Save original auto-exposure mode and manual exposure value
    double original_auto_exp = temp_cap.get(cv::CAP_PROP_AUTO_EXPOSURE);
    double original_exp = temp_cap.get(cv::CAP_PROP_EXPOSURE);

    temp_cap.set(cv::CAP_PROP_AUTO_EXPOSURE, 1);

    std::vector<cv::Mat> exposures;
    std::array<int, 3> exp_values = {linuxcampam::HDR_EXPOSURE_1,
                                     linuxcampam::HDR_EXPOSURE_2,
                                     linuxcampam::HDR_EXPOSURE_3};

    for (int exp : exp_values) {
      temp_cap.set(cv::CAP_PROP_EXPOSURE, exp);
      std::this_thread::sleep_for(
          std::chrono::milliseconds(linuxcampam::HDR_SETTLE_MS));

      for (int j = 0; j < linuxcampam::CAMERA_DRAIN_GRAB_COUNT; ++j) {
        temp_cap.grab();
      }

      cv::Mat exp_frame;
      if (is_ir_camera) {
        auto reader = [&temp_cap](cv::Mat &out) {
          return temp_cap.read(out);
        };
        exp_frame = linuxcampam::selectBestLitFrame(
            reader, linuxcampam::HDR_CANDIDATE_READS, /*ambient_subtraction=*/false);
      } else {
        temp_cap.read(frame);
        if (!frame.empty())
          exp_frame = frame.clone();
      }
      if (!exp_frame.empty())
        exposures.push_back(exp_frame);
    }

    // Restore original auto-exposure mode and exposure value
    temp_cap.set(cv::CAP_PROP_AUTO_EXPOSURE, original_auto_exp);
    if (original_exp > 0) {
      temp_cap.set(cv::CAP_PROP_EXPOSURE, original_exp);
    }

    if (exposures.size() >= static_cast<size_t>(linuxcampam::MIN_HDR_EXPOSURES)) {
      cv::Ptr<cv::MergeMertens> merge = cv::createMergeMertens();
      cv::Mat hdr;
      merge->process(exposures, hdr);
      cv::Mat result;
      hdr.convertTo(result, CV_8U, linuxcampam::HDR_BIT_DEPTH);
      log_info("[Camera] HDR merged " + std::to_string(exposures.size()) +
               " exposures on " + device_path);
      return result;
    }

    log_warn("[Camera] HDR capture failed to collect sufficient exposures on " + device_path);
    if (!exposures.empty()) {
      return exposures.at(exposures.size() / 2);
    }
  }

  // Averaging capture
  if (use_averaging && avg_frames > 1) {
    return captureAverageOnSession(temp_cap, avg_frames);
  }

  return captureSingleOnSession(temp_cap);
}

cv::Mat Camera::captureSingleOnSession(cv::VideoCapture &temp_cap) {
  cv::Mat frame;
  if (!is_ir_camera) {
    temp_cap.read(frame);
    return frame.empty() ? cv::Mat() : frame.clone();
  }

  auto reader = [&temp_cap](cv::Mat &out) {
    return temp_cap.read(out);
  };
  cv::Mat best = linuxcampam::selectBestLitFrame(
      reader, linuxcampam::kDefaultMaxCandidateReads, /*ambient_subtraction=*/false);
  if (best.empty()) {
    log_warn("[Camera] No illuminated frame detected for IR camera " + device_path);
    return cv::Mat();
  }
  return best;
}

cv::Mat Camera::captureAverageOnSession(cv::VideoCapture &temp_cap, int num_frames) {
  cv::Mat frame;
  auto reader = [&temp_cap](cv::Mat &out) {
    return temp_cap.read(out);
  };

  std::vector<cv::Mat> frames_to_average;
  if (is_ir_camera) {
    frames_to_average = linuxcampam::collectLitFrames(
        reader, num_frames, -1, /*ambient_subtraction=*/false);
  } else {
    frames_to_average.reserve(static_cast<size_t>(num_frames));
    for (int i = 0; i < num_frames; i++) {
      if (temp_cap.read(frame) && !frame.empty()) {
        frames_to_average.push_back(frame.clone());
      }
    }
  }

  if (frames_to_average.size() >= static_cast<size_t>(num_frames)) {
    log_info("[Camera] Averaged " + std::to_string(frames_to_average.size()) +
             " frames on " + device_path);
    return linuxcampam::averageFrames(frames_to_average);
  }

  log_warn("[Camera] Failed to collect sufficient frames for averaging (" +
           std::to_string(frames_to_average.size()) + "/" +
           std::to_string(num_frames) + ") on " + device_path);
  return cv::Mat();
}

cv::Mat Camera::captureHDR() {
  return captureEnrollment("on", true, linuxcampam::CAMERA_AVERAGE_FRAMES);
}
