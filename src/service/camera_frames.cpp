#include "camera_frames.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <opencv2/imgproc.hpp>

namespace linuxcampam {

namespace {
constexpr double kRgbChannelCount = 3.0;
constexpr double kSaturationPenaltyScale = 50.0;
constexpr double kTargetMidtoneBrightness = 60.0;
constexpr double kMinExposureScore = 0.1;
constexpr double kMaxBrightnessDistance = 120.0;
constexpr double kMidpointDivisor = 2.0;
} // namespace

cv::Mat subtractAmbient(const cv::Mat &lit_frame, const cv::Mat &dark_frame) {
  if (lit_frame.empty()) {
    return {};
  }
  if (dark_frame.empty() || lit_frame.size() != dark_frame.size()) {
    return lit_frame.clone();
  }

  cv::Mat matched_dark = dark_frame;
  if (lit_frame.channels() != dark_frame.channels()) {
    if (lit_frame.channels() == 3 && dark_frame.channels() == 1) {
      cv::cvtColor(dark_frame, matched_dark, cv::COLOR_GRAY2BGR);
    } else if (lit_frame.channels() == 1 && dark_frame.channels() == 3) {
      cv::cvtColor(dark_frame, matched_dark, cv::COLOR_BGR2GRAY);
    } else {
      return lit_frame.clone();
    }
  }

  if (matched_dark.type() != lit_frame.type()) {
    matched_dark.convertTo(matched_dark, lit_frame.type());
  }

  const double lit_mean = evaluateFrame(lit_frame).mean_brightness;
  const double dark_mean = evaluateFrame(matched_dark).mean_brightness;
  if (dark_mean >= lit_mean) {
    return lit_frame.clone();
  }

  cv::Mat diff;
  cv::subtract(lit_frame, matched_dark, diff);
  cv::max(diff, cv::Scalar::all(0), diff);
  return diff;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
FrameMetrics evaluateFrame(const cv::Mat &frame, double dark_threshold,
                           double max_saturation) {
  if (frame.empty()) {
    return {};
  }

  FrameMetrics m;
  const cv::Scalar mean_val = cv::mean(frame);
  std::array<double, 4> means{};
  std::copy(std::begin(mean_val.val), std::end(mean_val.val), means.begin());

  if (frame.channels() == 1) {
    m.mean_brightness = means.at(0);
  } else if (frame.channels() >= 3) {
    m.mean_brightness = (means.at(0) + means.at(1) + means.at(2)) / kRgbChannelCount;
  }

  m.is_lit = (m.mean_brightness >= dark_threshold);

  const int total_pixels = frame.rows * frame.cols;
  if (total_pixels > 0) {
    cv::Mat gray;
    if (frame.channels() == 1) {
      gray = frame;
    } else if (frame.channels() == 3) {
      cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    } else if (frame.channels() == 4) {
      cv::cvtColor(frame, gray, cv::COLOR_BGRA2GRAY);
    }

    int sat_pixels = cv::countNonZero(gray >= kSaturationPixelValue);
    m.saturation_ratio = static_cast<double>(sat_pixels) / static_cast<double>(total_pixels);
    m.is_overexposed = (m.saturation_ratio > max_saturation);
  }

  if (!m.is_lit) {
    m.quality_score = 0.0;
  } else {
    // Quality scoring: penalize saturation (glare/reflection blowout).
    // Ideal midtone balance is around kTargetMidtoneBrightness.
    double saturation_penalty = 1.0 / (1.0 + (m.saturation_ratio * kSaturationPenaltyScale));
    double distance_from_target = std::abs(m.mean_brightness - kTargetMidtoneBrightness);
    double exposure_score = std::max(kMinExposureScore, 1.0 - (distance_from_target / kMaxBrightnessDistance));
    m.quality_score = saturation_penalty * exposure_score;
  }

  return m;
}

CandidateBurst readCandidateBurst(const FrameReader &reader, int max_reads,
                                  double dark_threshold, double max_saturation) {
  CandidateBurst burst;
  if (!reader || max_reads <= 0) {
    burst.complete = false;
    return burst;
  }
  burst.candidates.reserve(static_cast<size_t>(max_reads));
  cv::Size expected_size;
  int expected_type = -1;

  for (int i = 0; i < max_reads; ++i) {
    cv::Mat frame;
    if (!reader(frame) || frame.empty()) {
      burst.complete = false;
      continue;
    }
    if (expected_type == -1) {
      expected_size = frame.size();
      expected_type = frame.type();
    } else if (frame.size() != expected_size || frame.type() != expected_type) {
      burst.complete = false;
      continue;
    }
    FrameMetrics m = evaluateFrame(frame, dark_threshold, max_saturation);
    burst.candidates.push_back({frame.clone(), m});
  }
  return burst;
}

PhasePartition partitionCandidates(const std::vector<Candidate> &candidates,
                                   double dark_threshold) {
  PhasePartition result;
  if (candidates.empty()) {
    result.classification = IlluminationClassification::Empty;
    return result;
  }

  double min_b = std::numeric_limits<double>::infinity();
  double max_b = -std::numeric_limits<double>::infinity();
  for (const auto &cand : candidates) {
    min_b = std::min(min_b, cand.metrics.mean_brightness);
    max_b = std::max(max_b, cand.metrics.mean_brightness);
  }

  // All frames below dark floor -> scene is completely dark
  if (max_b < dark_threshold) {
    result.classification = IlluminationClassification::Empty;
    return result;
  }

  const double delta = max_b - min_b;

  // Collect sorted brightness values to find the best strobe separation split
  std::vector<double> sorted_b;
  sorted_b.reserve(candidates.size());
  std::transform(candidates.begin(), candidates.end(), std::back_inserter(sorted_b),
                 [](const Candidate &c) { return c.metrics.mean_brightness; });
  std::sort(sorted_b.begin(), sorted_b.end());

  const size_t n = sorted_b.size();
  const size_t min_cluster_size = (n >= 4) ? 2 : 1;

  double best_gap = -1.0;
  double best_threshold = 0.0;

  for (size_t k = min_cluster_size; k + min_cluster_size <= n; ++k) {
    double max_low = sorted_b.at(k - 1);
    double min_low = sorted_b.front();
    double min_high = sorted_b.at(k);
    double max_high = sorted_b.back();
    double gap = min_high - max_low;
    double spread_low = max_low - min_low;
    double spread_high = max_high - min_high;

    const bool compact_groups = (spread_low <= kStableSpread) &&
                                (spread_high <= kStableSpread);

    if (compact_groups && gap >= kMinStrobeDelta && min_high >= dark_threshold) {
      if (gap > best_gap) {
        best_gap = gap;
        best_threshold = (max_low + min_high) / kMidpointDivisor;
      }
    }
  }

  if (best_gap >= kMinStrobeDelta) {
    result.classification = IlluminationClassification::SeparatedStrobe;
    for (const auto &cand : candidates) {
      if (cand.metrics.mean_brightness >= best_threshold) {
        result.lit_candidates.push_back(cand);
      } else {
        result.dark_candidates.push_back(cand);
      }
    }
    return result;
  }

  // Check for stable continuous illumination
  // Requires at least 2 frames to establish temporal stability; a single frame cannot prove continuous illumination
  if (n >= 2 && delta <= kStableSpread && min_b >= dark_threshold) {
    result.classification = IlluminationClassification::StableContinuous;
    result.lit_candidates = candidates;
    return result;
  }

  // Ambiguous burst: cannot separate phases, and not stable continuous.
  // Fail closed by rejecting the burst (empty lit_candidates).
  result.classification = IlluminationClassification::Ambiguous;
  return result;
}

IrFramePair selectBestLitPair(const FrameReader &reader, int max_reads,
                              double dark_threshold, double max_saturation) {
  if (!reader || max_reads <= 0) {
    return {};
  }

  auto burst = readCandidateBurst(reader, max_reads, dark_threshold, max_saturation);
  if (!burst.complete || burst.candidates.empty()) {
    return {};
  }

  auto partition = partitionCandidates(burst.candidates, dark_threshold);
  if (partition.lit_candidates.empty()) {
    return {};
  }

  IrFramePair pair;
  auto best_lit_it = std::max_element(
      partition.lit_candidates.begin(), partition.lit_candidates.end(),
      [](const Candidate &a, const Candidate &b) {
        return a.metrics.quality_score < b.metrics.quality_score;
      });
  if (best_lit_it != partition.lit_candidates.end()) {
    pair.lit = best_lit_it->frame.clone();
  }

  if (!partition.dark_candidates.empty()) {
    auto best_dark_it = std::min_element(
        partition.dark_candidates.begin(), partition.dark_candidates.end(),
        [](const Candidate &a, const Candidate &b) {
          return a.metrics.mean_brightness < b.metrics.mean_brightness;
        });
    if (best_dark_it != partition.dark_candidates.end()) {
      pair.dark = best_dark_it->frame.clone();
      pair.has_dark = true;
    }
  }

  return pair;
}

cv::Mat selectBestLitFrame(const FrameReader &reader, int max_reads,
                           bool ambient_subtraction,
                           double dark_threshold, double max_saturation) {
  IrFramePair pair = selectBestLitPair(reader, max_reads, dark_threshold, max_saturation);
  if (pair.lit.empty()) {
    return {};
  }
  if (ambient_subtraction && pair.has_dark) {
    return subtractAmbient(pair.lit, pair.dark);
  }
  return pair.lit;
}

std::vector<cv::Mat> collectLitFrames(const FrameReader &reader, int wanted_frames,
                                      int max_reads, bool ambient_subtraction,
                                      double dark_threshold,
                                      double max_saturation) {
  if (!reader || wanted_frames <= 0) {
    return {};
  }
  if (max_reads <= 0) {
    max_reads = std::max(static_cast<int>(kDefaultMaxCandidateReads),
                         wanted_frames * 2 + 2);
  }
  constexpr int kHardMaxReads = 32;
  max_reads = std::min(max_reads, kHardMaxReads);

  auto burst = readCandidateBurst(reader, max_reads, dark_threshold, max_saturation);
  if (!burst.complete || burst.candidates.empty()) {
    return {};
  }

  auto partition = partitionCandidates(burst.candidates, dark_threshold);
  if (partition.lit_candidates.empty()) {
    return {};
  }

  cv::Mat dark_ref;
  if (ambient_subtraction && !partition.dark_candidates.empty()) {
    auto best_dark_it = std::min_element(
        partition.dark_candidates.begin(), partition.dark_candidates.end(),
        [](const Candidate &a, const Candidate &b) {
          return a.metrics.mean_brightness < b.metrics.mean_brightness;
        });
    if (best_dark_it != partition.dark_candidates.end()) {
      dark_ref = best_dark_it->frame.clone();
    }
  }

  std::vector<cv::Mat> result;
  const size_t count = std::min(static_cast<size_t>(wanted_frames),
                                partition.lit_candidates.size());
  result.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const auto &lit_mat = partition.lit_candidates.at(i).frame;
    if (ambient_subtraction && !dark_ref.empty()) {
      result.push_back(subtractAmbient(lit_mat, dark_ref));
    } else {
      result.push_back(lit_mat.clone());
    }
  }

  return result;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

cv::Mat averageFrames(const std::vector<cv::Mat> &frames) {
  if (frames.empty()) {
    return {};
  }
  if (frames.size() == 1) {
    return frames.front().clone();
  }

  cv::Mat sum;
  int count = 0;
  const cv::Size expected_size = frames.front().size();
  const int expected_type = frames.front().type();

  for (const auto &frame : frames) {
    if (frame.empty() || frame.size() != expected_size || frame.type() != expected_type) {
      continue;
    }

    if (count == 0) {
      frame.convertTo(sum, CV_32F);
      count++;
    } else {
      cv::Mat f32;
      frame.convertTo(f32, CV_32F);
      cv::add(sum, f32, sum);
      count++;
    }
  }

  if (count == 0 || sum.empty()) {
    return {};
  }

  sum /= static_cast<float>(count);
  cv::Mat result;
  sum.convertTo(result, expected_type);
  cv::max(result, cv::Scalar::all(0), result);
  return result;
}

} // namespace linuxcampam
