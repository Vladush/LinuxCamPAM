#pragma once

#include <cstdint>
#include <functional>
#include <opencv2/core.hpp>
#include <vector>

namespace linuxcampam {

inline constexpr double kDefaultIrDarkFrameMean = 3.0;
inline constexpr double kDefaultMaxSaturationRatio = 0.02; // 2% clipping
inline constexpr int kSaturationPixelValue = 250;
inline constexpr int kDefaultMaxCandidateReads = 6;
inline constexpr double kMinStrobeDelta = 8.0;
inline constexpr double kMinPhaseSeparation = 4.0;
inline constexpr double kStableSpread = 4.0;
inline constexpr double kMaxSpreadFraction = 0.4;

struct FrameMetrics {
  double mean_brightness = 0.0;
  double saturation_ratio = 0.0;
  bool is_lit = false;
  bool is_overexposed = false;
  double quality_score = 0.0;
};

enum class IlluminationClassification : std::uint8_t {
  Empty,
  SeparatedStrobe,
  StableContinuous,
  Ambiguous
};

struct Candidate {
  cv::Mat frame;
  FrameMetrics metrics;
};

struct PhasePartition {
  IlluminationClassification classification = IlluminationClassification::Empty;
  std::vector<Candidate> lit_candidates;
  std::vector<Candidate> dark_candidates;
};

// Pure function: partitions candidate frames into illuminated and ambient phases.
// Distinguishes separated strobe, stable continuous, and ambiguous illumination.
// Fails closed (empty lit_candidates) on ambiguous bursts.
[[nodiscard]] PhasePartition partitionCandidates(
    const std::vector<Candidate> &candidates,
    double dark_threshold = kDefaultIrDarkFrameMean);

struct IrFramePair {
  cv::Mat lit{};
  cv::Mat dark{};
  bool has_dark = false;
};

// Pure function: subtracts ambient dark frame from illuminated lit frame using saturating subtraction.
// Returns a clone of lit_frame if dark_frame is empty, invalid, or mismatched.
[[nodiscard]] cv::Mat subtractAmbient(const cv::Mat &lit_frame,
                                      const cv::Mat &dark_frame);

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
// Evaluates exposure and illumination metrics for a frame.
// Supports 1-channel (GREY), 3-channel (BGR), and 4-channel (BGRA) frames.
[[nodiscard]] FrameMetrics evaluateFrame(
    const cv::Mat &frame,
    double dark_threshold = kDefaultIrDarkFrameMean,
    double max_saturation = kDefaultMaxSaturationRatio);

using FrameReader = std::function<bool(cv::Mat &)>;

// Reads candidate frames and retains both the best lit frame (IR_ON) and ambient frame (IR_OFF).
[[nodiscard]] IrFramePair selectBestLitPair(
    const FrameReader &reader,
    int max_reads = kDefaultMaxCandidateReads,
    double dark_threshold = kDefaultIrDarkFrameMean,
    double max_saturation = kDefaultMaxSaturationRatio);

// Reads consecutive frames up to max_reads times to capture at least one IR_ON phase.
// Selects the best lit frame and optionally performs ambient IR subtraction (default: false)
// if an IR_OFF dark frame is available.
[[nodiscard]] cv::Mat selectBestLitFrame(
    const FrameReader &reader,
    int max_reads = kDefaultMaxCandidateReads,
    bool ambient_subtraction = false,
    double dark_threshold = kDefaultIrDarkFrameMean,
    double max_saturation = kDefaultMaxSaturationRatio);

// Collects up to wanted_frames valid, illuminated frames, skipping dark IR_OFF frames.
// When ambient_subtraction is true, subtracts detected ambient IR from lit frames.
// Enforces a max_reads bound to prevent infinite loops.
[[nodiscard]] std::vector<cv::Mat> collectLitFrames(
    const FrameReader &reader,
    int wanted_frames,
    int max_reads = -1,
    bool ambient_subtraction = false,
    double dark_threshold = kDefaultIrDarkFrameMean,
    double max_saturation = kDefaultMaxSaturationRatio);
// NOLINTEND(bugprone-easily-swappable-parameters)

// Averages a collection of valid frames. Returns an empty cv::Mat if empty.
[[nodiscard]] cv::Mat averageFrames(const std::vector<cv::Mat> &frames);

} // namespace linuxcampam
