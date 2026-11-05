// Runs the production face verification code once per line read from stdin and
// writes one JSON result per line, with each camera's frame and score.
//
//   tools/ir-test/build.sh
//   echo scan | sudo tools/ir-test/live_scan <username> [config.ini]
//
// It links the same service_core library as linuxcampamd, so capture,
// detection, matching and the camera policy are the real code. It runs in its
// own process, so the daemon's lockout count isn't affected and nothing is
// written to disk.
//
// One difference from a real login: the daemon's proximity enforcement lives in
// its main loop, not the engine, so it isn't applied here.

#include "json.hpp"
#include "service/auth_engine.hpp"
#include "service/camera_frames.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

constexpr const char *kDefaultConfig = "/etc/linuxcampam/config.ini";
constexpr int kPreviewMaxWidth = 640;
constexpr int kPreviewJpegQuality = 80;
constexpr int kYuNetScoreColumn =
    14; // Matches generateEmbedding() in auth_engine.cpp.

std::string base64(const std::vector<uchar> &bytes) {
  static constexpr std::array<char, 65> kAlphabet{
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const size_t left = bytes.size() - i;
    const uint32_t chunk =
        (static_cast<uint32_t>(bytes[i]) << 16U) |
        (left > 1 ? static_cast<uint32_t>(bytes[i + 1]) << 8U : 0U) |
        (left > 2 ? static_cast<uint32_t>(bytes[i + 2]) : 0U);
    out += kAlphabet[(chunk >> 18U) & 63U];
    out += kAlphabet[(chunk >> 12U) & 63U];
    out += left > 1 ? kAlphabet[(chunk >> 6U) & 63U] : '=';
    out += left > 2 ? kAlphabet[chunk & 63U] : '=';
  }
  return out;
}

std::string encodePreview(const cv::Mat &frame) {
  cv::Mat preview = frame;
  if (frame.cols > kPreviewMaxWidth) {
    const double scale = static_cast<double>(kPreviewMaxWidth) / frame.cols;
    cv::resize(frame, preview, cv::Size(), scale, scale, cv::INTER_AREA);
  }
  std::vector<uchar> jpeg;
  cv::imencode(".jpg", preview, jpeg,
               {cv::IMWRITE_JPEG_QUALITY, kPreviewJpegQuality});
  return base64(jpeg);
}

const char *policyName(Configuration::AuthPolicy policy) {
  switch (policy) {
  case Configuration::AuthPolicy::STRICT_ALL:
    return "strict";
  case Configuration::AuthPolicy::LENIENT_ANY:
    return "lenient";
  case Configuration::AuthPolicy::ADAPTIVE:
    return "adaptive";
  }
  return "unknown";
}

json failure(const std::string &reason) {
  return {
      {"would_pass", false}, {"reason", reason}, {"cameras", json::array()}};
}

void writeLine(int fd, const json &value) {
  const std::string line =
      value.dump(-1, ' ', false, json::error_handler_t::replace) + '\n';
  size_t written = 0;
  while (written < line.size()) {
    const ssize_t count =
        write(fd, line.data() + written, line.size() - written);
    if (count <= 0)
      std::exit(1);
    written += static_cast<size_t>(count);
  }
}

} // namespace

// auth_engine.hpp befriends this class name when UNIT_TESTING is defined, the
// same access the unit tests use, which reaches verifyUserCore() without
// changing the engine.
class AuthEngineTest {
public:
  static json scan(AuthEngine &engine, const std::string &user) {
    struct Capture {
      std::string id;
      cv::Mat frame;
      bool match = false;
      float score = 0.0F;
      std::string message;
      long long ms = 0;
    };
    std::vector<Capture> captures;
    const auto started = Clock::now();
    auto last = started;
    auto millis = [](Clock::duration span) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(span)
          .count();
    };

    // The callback runs inside the verification deadline, so it only records.
    // Everything slow happens after the result is decided.
    const AuthResult result = engine.verifyUserCore(
        user, [&](const std::string &id, const cv::Mat &frame, bool match,
                  float score, const std::string &message) {
          const auto now = Clock::now();
          captures.push_back(
              {id, frame, match, score, message, millis(now - last)});
          last = now;
        });
    const auto elapsed = millis(Clock::now() - started);

    const Configuration &config = engine.getConfig();
    json cameras = json::array();
    for (const Capture &capture : captures) {
      json camera = {{"id", capture.id},       {"match", capture.match},
                     {"score", capture.score}, {"message", capture.message},
                     {"ms", capture.ms},       {"faces", json::array()}};
      const auto def = std::find_if(
          engine.active_cameras.begin(), engine.active_cameras.end(),
          [&](const AuthEngine::ActiveCamera &active) {
            return active.config.id == capture.id;
          });
      if (def != engine.active_cameras.end()) {
        camera["type"] = def->config.type;
        camera["mandatory"] = def->config.mandatory;
      }
      if (!capture.frame.empty()) {
        const auto metrics = linuxcampam::evaluateFrame(capture.frame);
        camera["brightness"] = metrics.mean_brightness;
        camera["clipped"] = metrics.saturation_ratio;
        camera["width"] = capture.frame.cols;
        camera["height"] = capture.frame.rows;
        camera["image"] = encodePreview(capture.frame);
        // Only for drawing boxes: the verdict above was already decided. Row 0
        // is the face the engine matched.
        if (capture.message == "MATCH" || capture.message == "NO MATCH") {
          cv::Mat faces;
          (void)engine.detectFaces(capture.frame, faces);
          for (int row = 0; row < faces.rows; ++row) {
            camera["faces"].push_back(
                {{"x", faces.at<float>(row, 0)},
                 {"y", faces.at<float>(row, 1)},
                 {"w", faces.at<float>(row, 2)},
                 {"h", faces.at<float>(row, 3)},
                 {"score", faces.at<float>(row, kYuNetScoreColumn)}});
          }
        }
      }
      cameras.push_back(std::move(camera));
    }

    return {{"would_pass", result.success},
            {"reason", result.reason},
            {"best_score", result.best_score},
            {"threshold", config.threshold},
            {"detection_threshold", config.detection_threshold},
            {"policy", policyName(config.policy)},
            {"timeout_ms", config.timeout_ms},
            {"elapsed_ms", elapsed},
            {"cameras", std::move(cameras)}};
  }
};

int main(int argc, char **argv) {
  if (argc < 2 || argc > 3) {
    std::cerr
        << "Usage: sudo live_scan <username> [config.ini]\n"
        << "Writes one JSON result to stdout for each line read from stdin.\n";
    return 2;
  }
  const std::string user = argv[1];
  const std::string config_path = argc == 3 ? argv[2] : kDefaultConfig;
  if (geteuid() != 0) {
    std::cerr
        << "Warning: not running as root, so enrolled faces in the default "
           "users directory can't be read.\n";
  }

  // The engine's logger writes to stdout. Keep the real stdout for JSON only.
  const int json_fd = dup(STDOUT_FILENO);
  if (json_fd < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
    return 1;

  AuthEngine engine;
  const bool ready = engine.init(config_path, true);

  std::string request;
  while (std::getline(std::cin, request)) {
    if (!ready) {
      writeLine(json_fd, failure("Couldn't load " + config_path +
                                 " or the face models"));
      continue;
    }
    try {
      writeLine(json_fd, AuthEngineTest::scan(engine, user));
    } catch (const std::exception &error) {
      writeLine(json_fd, failure(std::string("Scan failed: ") + error.what()));
    }
  }
  return 0;
}
