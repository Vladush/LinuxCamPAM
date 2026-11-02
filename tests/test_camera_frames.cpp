#include "service/camera_frames.hpp"
#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <vector>

namespace {

constexpr int kTestWidth = 100;
constexpr int kTestHeight = 100;
cv::Size testSize() { return {kTestWidth, kTestHeight}; }

constexpr int kSmallDim = 50;
cv::Size smallSize() { return {kSmallDim, kSmallDim}; }

constexpr int kMicroDim = 10;
cv::Size microSize() { return {kMicroDim, kMicroDim}; }

constexpr uint8_t kDarkVal = 1;
constexpr uint8_t kSunlightAmbient = 20;
constexpr uint8_t kLitVal35 = 35;
constexpr uint8_t kLitVal40 = 40;
constexpr uint8_t kLitVal44 = 44;
constexpr uint8_t kLitVal45 = 45;
constexpr uint8_t kLitVal50 = 50;
constexpr uint8_t kLitVal55 = 55;
constexpr uint8_t kLitVal60 = 60;
constexpr uint8_t kLitVal61 = 61;
constexpr uint8_t kLitVal70 = 70;
constexpr uint8_t kLitVal80 = 80;
constexpr uint8_t kLitVal81 = 81;
constexpr uint8_t kLitVal86 = 86;
constexpr uint8_t kLitVal90 = 90;
constexpr uint8_t kLitVal100 = 100;
constexpr uint8_t kLitVal120 = 120;
constexpr uint8_t kLitVal121 = 121;
constexpr uint8_t kLitVal122 = 122;
constexpr uint8_t kLitVal160 = 160;
constexpr uint8_t kLitVal220 = 220;
constexpr uint8_t kMaxPixelVal = 255;
constexpr float kFloatLowVal = 10.0f;
constexpr float kFloatHighVal = 50.0f;
constexpr float kFloatDarkVal = 20.0f;
constexpr uint8_t kUnderflowLow = 10;
constexpr uint8_t kUnderflowHigh = 30;

constexpr int kReads5 = 5;
constexpr int kReads6 = 6;

constexpr double kGlareFraction01 = 0.01;
constexpr double kGlareFraction10 = 0.10;

// Helper to create synthetic solid test frames
cv::Mat makeSolidFrame(cv::Size size, uint8_t value) {
  return cv::Mat(size, CV_8UC1, cv::Scalar::all(value));
}

// Helper to create a frame with a blown-out specular highlight
cv::Mat makeGlareFrame(double glare_fraction, cv::Size size, uint8_t base_val) {
  cv::Mat frame = makeSolidFrame(size, base_val);
  int glare_rows = static_cast<int>(size.height * glare_fraction);
  if (glare_rows > 0) {
    frame.rowRange(0, glare_rows).setTo(cv::Scalar(kMaxPixelVal));
  }
  return frame;
}

// Helper to extract first channel mean brightness without operator[]
double meanBrightness(const cv::Mat &mat) {
  return cv::mean(mat).val[0];
}

// Sequence generator simulating camera frame reads
class SequenceReader {
public:
  explicit SequenceReader(std::vector<cv::Mat> sequence)
      : sequence_(std::move(sequence)) {}

  bool operator()(cv::Mat &out_frame) {
    if (idx_ < sequence_.size()) {
      out_frame = sequence_.at(idx_++).clone();
      return true;
    }
    out_frame = cv::Mat();
    return false;
  }

  size_t readsPerformed() const { return idx_; }

private:
  std::vector<cv::Mat> sequence_{};
  size_t idx_ = 0;
};

} // namespace

TEST(CameraFramesTest, EvaluateEmptyFrame) {
  cv::Mat empty;
  auto m = linuxcampam::evaluateFrame(empty);
  EXPECT_FALSE(m.is_lit);
  EXPECT_FALSE(m.is_overexposed);
  EXPECT_DOUBLE_EQ(m.mean_brightness, 0.0);
  EXPECT_DOUBLE_EQ(m.saturation_ratio, 0.0);
  EXPECT_DOUBLE_EQ(m.quality_score, 0.0);
}

TEST(CameraFramesTest, EvaluateDarkAndLitFrames) {
  // Typical IR_OFF ambient dark frame
  cv::Mat dark_frame = makeSolidFrame(testSize(), kDarkVal);
  auto dark_m = linuxcampam::evaluateFrame(dark_frame);
  EXPECT_FALSE(dark_m.is_lit);
  EXPECT_DOUBLE_EQ(dark_m.quality_score, 0.0);

  // Typical IR_ON lit frame
  cv::Mat lit_frame = makeSolidFrame(testSize(), kLitVal50);
  auto lit_m = linuxcampam::evaluateFrame(lit_frame);
  EXPECT_TRUE(lit_m.is_lit);
  EXPECT_FALSE(lit_m.is_overexposed);
  EXPECT_GT(lit_m.quality_score, 0.5);
}

TEST(CameraFramesTest, EvaluateOverexposureAndSaturation) {
  // Frame with 10% saturated pixels (severe specular glare on forehead/glasses)
  cv::Mat glare_frame = makeGlareFrame(kGlareFraction10, testSize(), kLitVal45);
  auto m = linuxcampam::evaluateFrame(glare_frame);

  EXPECT_TRUE(m.is_lit);
  EXPECT_TRUE(m.is_overexposed);
  EXPECT_NEAR(m.saturation_ratio, kGlareFraction10, 0.01);
  // Quality score must be heavily penalized due to saturation
  EXPECT_LT(m.quality_score, 0.2);
}

TEST(CameraFramesTest, SubtractAmbientRemovesSunlightBackground) {
  // Sunlight creates ambient illumination across the whole frame (val=20).
  // Active emitter illuminates the face region adding 40 (total 60).
  cv::Mat lit_frame = makeSolidFrame(testSize(), kLitVal60);
  cv::Mat dark_frame = makeSolidFrame(testSize(), kSunlightAmbient);

  cv::Mat diff = linuxcampam::subtractAmbient(lit_frame, dark_frame);
  ASSERT_FALSE(diff.empty());
  EXPECT_EQ(diff.size(), testSize());
  EXPECT_NEAR(meanBrightness(diff), 40.0, 0.1);
}

TEST(CameraFramesTest, SubtractAmbientHandlesMismatchedOrEmptyFrames) {
  cv::Mat lit_frame = makeSolidFrame(testSize(), kLitVal40);
  cv::Mat mismatched_frame = makeSolidFrame(smallSize(), kSunlightAmbient);
  cv::Mat empty_frame;

  // Empty dark frame falls back to clone of lit frame
  cv::Mat fallback_empty = linuxcampam::subtractAmbient(lit_frame, empty_frame);
  EXPECT_NEAR(meanBrightness(fallback_empty), 40.0, 0.1);

  // Mismatched size falls back to clone of lit frame
  cv::Mat fallback_mismatch = linuxcampam::subtractAmbient(lit_frame, mismatched_frame);
  EXPECT_NEAR(meanBrightness(fallback_mismatch), 40.0, 0.1);

  // Empty lit frame returns empty
  cv::Mat fallback_lit_empty = linuxcampam::subtractAmbient(empty_frame, lit_frame);
  EXPECT_TRUE(fallback_lit_empty.empty());
}

TEST(CameraFramesTest, StrobePhaseSelectionPairRetention) {
  // Verify that selectBestLitPair retains both IR_ON and IR_OFF frames
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kDarkVal),
      makeSolidFrame(testSize(), kLitVal45),
      makeSolidFrame(testSize(), kDarkVal),
      makeSolidFrame(testSize(), kLitVal44)};

  SequenceReader reader(stream);
  auto pair = linuxcampam::selectBestLitPair(
      [&](cv::Mat &f) { return reader(f); }, 4);

  ASSERT_FALSE(pair.lit.empty());
  ASSERT_TRUE(pair.has_dark);
  ASSERT_FALSE(pair.dark.empty());
  EXPECT_NEAR(meanBrightness(pair.lit), 45.0, 0.5);
  EXPECT_NEAR(meanBrightness(pair.dark), 1.0, 0.5);
}

TEST(CameraFramesTest, StrobePhaseSelectionAmbientSubtractionEnabled) {
  // Ambient subtraction (on by default) subtracts ambient dark frame (val=1) from lit frame (val=40) -> 39
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kLitVal40),
      makeSolidFrame(testSize(), kDarkVal),
      makeSolidFrame(testSize(), kLitVal40),
      makeSolidFrame(testSize(), kDarkVal)};

  SequenceReader reader(stream);
  cv::Mat selected = linuxcampam::selectBestLitFrame(
      [&](cv::Mat &f) { return reader(f); }, 4, /*ambient_subtraction=*/true);

  ASSERT_FALSE(selected.empty());
  EXPECT_NEAR(meanBrightness(selected), 39.0, 0.5);
}

TEST(CameraFramesTest, StrobePhaseSelectionAmbientSubtractionDisabled) {
  // With ambient subtraction disabled, lit frame is returned as-is (val=40)
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kLitVal40),
      makeSolidFrame(testSize(), kDarkVal),
      makeSolidFrame(testSize(), kLitVal40),
      makeSolidFrame(testSize(), kDarkVal)};

  SequenceReader reader(stream);
  cv::Mat selected = linuxcampam::selectBestLitFrame(
      [&](cv::Mat &f) { return reader(f); }, 4, /*ambient_subtraction=*/false);

  ASSERT_FALSE(selected.empty());
  EXPECT_NEAR(meanBrightness(selected), 40.0, 0.5);
}

TEST(CameraFramesTest, StrobePhaseSelectionInverted) {
  // Inverted pattern starting on OFF: [OFF (val=1), ON (val=45), OFF (val=1), ON (val=45)]
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kDarkVal),
      makeSolidFrame(testSize(), kLitVal45),
      makeSolidFrame(testSize(), kDarkVal),
      makeSolidFrame(testSize(), kLitVal45)};

  SequenceReader reader(stream);
  cv::Mat selected = linuxcampam::selectBestLitFrame(
      [&](cv::Mat &f) { return reader(f); }, 4, /*ambient_subtraction=*/true);

  ASSERT_FALSE(selected.empty());
  EXPECT_NEAR(meanBrightness(selected), 44.0, 0.5);
}

TEST(CameraFramesTest, AvoidsOverexposedFrameInBurst) {
  // In a burst:
  // Frame 0: blown-out glare frame (base=55, 1% saturated at 255) -> higher raw mean, glare penalty
  // Frame 1: well-balanced frame (solid 55, 0% saturated) -> lower raw mean, no glare
  // Frame 2: pitch-black OFF frame (solid 1)
  cv::Mat blown_out = makeGlareFrame(kGlareFraction01, testSize(), kLitVal55);
  cv::Mat balanced = makeSolidFrame(testSize(), kLitVal55);
  cv::Mat dark = makeSolidFrame(testSize(), kDarkVal);

  EXPECT_GT(meanBrightness(blown_out), meanBrightness(balanced));

  std::vector<cv::Mat> stream = {blown_out, balanced, dark};
  SequenceReader reader(stream);

  cv::Mat selected = linuxcampam::selectBestLitFrame(
      [&](cv::Mat &f) { return reader(f); }, 3, /*ambient_subtraction=*/false);

  ASSERT_FALSE(selected.empty());
  // Must select the balanced frame, NOT the blown-out one
  EXPECT_NEAR(meanBrightness(selected), 55.0, 0.5);
}

TEST(CameraFramesTest, FailsClosedWhenEmitterDoesNotFire) {
  // All frames are pitch-black (emitter didn't fire)
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kDarkVal),
      makeSolidFrame(testSize(), kDarkVal),
      makeSolidFrame(testSize(), kDarkVal),
      makeSolidFrame(testSize(), kDarkVal)};

  SequenceReader reader(stream);
  cv::Mat selected = linuxcampam::selectBestLitFrame(
      [&](cv::Mat &f) { return reader(f); }, 4);

  EXPECT_TRUE(selected.empty());
}

TEST(CameraFramesTest, CollectLitFramesWithAmbientSubtraction) {
  // Stream: [ON1=44, OFF=1, ON2=45, OFF=1, ON3=45, OFF=1]
  // Ambient subtraction subtracts ambient dark frame (val=1) -> 43 and 44
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kLitVal44),
      makeSolidFrame(testSize(), kDarkVal),
      makeSolidFrame(testSize(), kLitVal45),
      makeSolidFrame(testSize(), kDarkVal),
      makeSolidFrame(testSize(), kLitVal45),
      makeSolidFrame(testSize(), kDarkVal)};

  SequenceReader reader(stream);
  auto collected = linuxcampam::collectLitFrames(
      [&](cv::Mat &f) { return reader(f); }, 2, kReads6, /*ambient_subtraction=*/true);

  ASSERT_EQ(collected.size(), 2);
  EXPECT_NEAR(meanBrightness(collected.at(0)), 43.0, 0.5);
  EXPECT_NEAR(meanBrightness(collected.at(1)), 44.0, 0.5);
}

TEST(CameraFramesTest, CollectLitFramesWithoutAmbientSubtraction) {
  // With ambient subtraction disabled, lit frames are returned un-subtracted
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kLitVal44),
      makeSolidFrame(testSize(), kDarkVal),
      makeSolidFrame(testSize(), kLitVal45),
      makeSolidFrame(testSize(), kDarkVal),
      makeSolidFrame(testSize(), kLitVal45),
      makeSolidFrame(testSize(), kDarkVal)};

  SequenceReader reader(stream);
  auto collected = linuxcampam::collectLitFrames(
      [&](cv::Mat &f) { return reader(f); }, 2, kReads6, /*ambient_subtraction=*/false);

  ASSERT_EQ(collected.size(), 2);
  EXPECT_NEAR(meanBrightness(collected.at(0)), 44.0, 0.5);
  EXPECT_NEAR(meanBrightness(collected.at(1)), 45.0, 0.5);
}

TEST(CameraFramesTest, CollectLitFramesTerminatesOnDarkStream) {
  // Stream of only dark frames must terminate via max_reads bound
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kDarkVal),
      makeSolidFrame(testSize(), kDarkVal),
      makeSolidFrame(testSize(), kDarkVal),
      makeSolidFrame(testSize(), kDarkVal)};

  SequenceReader reader(stream);
  auto collected = linuxcampam::collectLitFrames(
      [&](cv::Mat &f) { return reader(f); }, 3, 4);

  EXPECT_TRUE(collected.empty());
  EXPECT_EQ(reader.readsPerformed(), 4);
}

TEST(CameraFramesTest, AverageFramesMathematicalAccuracy) {
  // Test averaging two frames of 40 and 60 -> expected 50
  cv::Mat f1 = makeSolidFrame(smallSize(), kLitVal40);
  cv::Mat f2 = makeSolidFrame(smallSize(), kLitVal60);

  cv::Mat avg = linuxcampam::averageFrames({f1, f2});
  ASSERT_FALSE(avg.empty());
  EXPECT_EQ(avg.size(), smallSize());
  EXPECT_NEAR(meanBrightness(avg), 50.0, 0.1);

  // Single frame returns clone
  cv::Mat single_avg = linuxcampam::averageFrames({f1});
  EXPECT_NEAR(meanBrightness(single_avg), 40.0, 0.1);

  // Empty vector returns empty Mat
  cv::Mat empty_avg = linuxcampam::averageFrames({});
  EXPECT_TRUE(empty_avg.empty());
}

TEST(CameraFramesTest, SubtractAmbientGuaranteesNoNegativeValuesOnFloatingPoint) {
  // Test that when floating point frames produce negative difference (10.0 - 20.0),
  // subtractAmbient strictly clamps all negative values to 0.0f
  cv::Mat lit_frame(smallSize(), CV_32FC1, cv::Scalar(kFloatLowVal));
  // Set second half of frame to higher value (50.0f) so mean(lit) > mean(dark)
  lit_frame.rowRange(smallSize().height / 2, smallSize().height).setTo(cv::Scalar(kFloatHighVal));

  cv::Mat dark_frame(smallSize(), CV_32FC1, cv::Scalar(kFloatDarkVal));

  cv::Mat diff = linuxcampam::subtractAmbient(lit_frame, dark_frame);
  ASSERT_FALSE(diff.empty());
  EXPECT_EQ(diff.type(), CV_32FC1);

  // The first half: 10.0f - 20.0f would be -10.0f without non-negative clamping
  float val_clamped = diff.at<float>(0, 0);
  EXPECT_FLOAT_EQ(val_clamped, 0.0f);

  // The second half: 50.0f - 20.0f = 30.0f
  float val_positive = diff.at<float>(smallSize().height - 1, 0);
  EXPECT_FLOAT_EQ(val_positive, 30.0f);

  // Verify that strictly no negative values exist in the entire matrix
  double min_val = 0.0;
  double max_val = 0.0;
  cv::minMaxLoc(diff, &min_val, &max_val);
  EXPECT_GE(min_val, 0.0);
}

TEST(CameraFramesTest, SubtractAmbientGuaranteesNoUnderflowWrapAroundOn8Bit) {
  // In 8-bit unsigned, if a pixel in dark frame is brighter than lit frame,
  // it must saturate to 0 rather than wrapping around to 256-N
  cv::Mat lit_frame = makeSolidFrame(smallSize(), kUnderflowLow);
  lit_frame.rowRange(smallSize().height / 2, smallSize().height).setTo(cv::Scalar(kLitVal60));

  cv::Mat dark_frame = makeSolidFrame(smallSize(), kUnderflowHigh);

  cv::Mat diff = linuxcampam::subtractAmbient(lit_frame, dark_frame);
  ASSERT_FALSE(diff.empty());

  // First half: 10 - 30 saturates to 0, does not underflow to 236
  uint8_t clamped_pixel = diff.at<uint8_t>(0, 0);
  EXPECT_EQ(clamped_pixel, 0);

  // Second half: 60 - 30 = 30
  uint8_t positive_pixel = diff.at<uint8_t>(smallSize().height - 1, 0);
  EXPECT_EQ(positive_pixel, 30);
}

TEST(CameraFramesTest, SubtractAmbientRejectsDarkFrameBrighterThanLitFrame) {
  // If ambient dark frame is overall brighter than lit frame,
  // it cannot be valid ambient background. Must fall back to lit frame untouched.
  cv::Mat lit_frame = makeSolidFrame(smallSize(), kLitVal35);
  cv::Mat dark_frame = makeSolidFrame(smallSize(), kLitVal50);

  cv::Mat diff = linuxcampam::subtractAmbient(lit_frame, dark_frame);
  ASSERT_FALSE(diff.empty());
  EXPECT_NEAR(meanBrightness(diff), 35.0, 0.1);
}

TEST(CameraFramesTest, SubtractAmbientHandlesChannelConversionGracefully) {
  // 3-channel BGR lit frame (val 50) and 1-channel grayscale dark frame (val 20)
  cv::Mat lit_frame(smallSize(), CV_8UC3, cv::Scalar::all(kLitVal50));
  cv::Mat dark_frame(smallSize(), CV_8UC1, cv::Scalar::all(kSunlightAmbient));

  cv::Mat diff = linuxcampam::subtractAmbient(lit_frame, dark_frame);
  ASSERT_FALSE(diff.empty());
  EXPECT_EQ(diff.channels(), 3);
  EXPECT_NEAR(meanBrightness(diff), 30.0, 0.5);
}

TEST(CameraFramesTest, StrobePhaseSelectionInSunlightAmbient) {
  // Ambient sunlight elevates OFF frame to 20 (> 3.0 dark_threshold).
  // Active IR strobe adds 40 (total 60).
  // Stream: [OFF=20, ON=60, OFF=20, ON=60]
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kSunlightAmbient),
      makeSolidFrame(testSize(), kLitVal60),
      makeSolidFrame(testSize(), kSunlightAmbient),
      makeSolidFrame(testSize(), kLitVal60)};

  SequenceReader reader(stream);
  auto pair = linuxcampam::selectBestLitPair(
      [&](cv::Mat &f) { return reader(f); }, 4);

  ASSERT_FALSE(pair.lit.empty());
  ASSERT_TRUE(pair.has_dark);
  ASSERT_FALSE(pair.dark.empty());
  EXPECT_NEAR(meanBrightness(pair.lit), 60.0, 0.5);
  EXPECT_NEAR(meanBrightness(pair.dark), 20.0, 0.5);

  cv::Mat subtracted = linuxcampam::subtractAmbient(pair.lit, pair.dark);
  ASSERT_FALSE(subtracted.empty());
  EXPECT_NEAR(meanBrightness(subtracted), 40.0, 0.5);

  double min_val = 0.0;
  double max_val = 0.0;
  cv::minMaxLoc(subtracted, &min_val, &max_val);
  EXPECT_GE(min_val, 0.0);
}

TEST(CameraFramesTest, BrightAmbientCannotWinExposureRanking) {
  // Bright ambient makes OFF=80, and ON=120.
  // 80 is closer to target midtone (60) than 120, but 120 is the active lit phase.
  // The classifier must never choose OFF over ON.
  constexpr uint8_t kBrightOff = 80;
  constexpr uint8_t kBrightOn = 120;
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kBrightOff),
      makeSolidFrame(testSize(), kBrightOn),
      makeSolidFrame(testSize(), kBrightOff),
      makeSolidFrame(testSize(), kBrightOn)};

  SequenceReader reader(stream);
  auto pair = linuxcampam::selectBestLitPair(
      [&](cv::Mat &f) { return reader(f); }, 4);

  ASSERT_FALSE(pair.lit.empty());
  ASSERT_TRUE(pair.has_dark);
  EXPECT_NEAR(meanBrightness(pair.lit), 120.0, 0.5);
  EXPECT_NEAR(meanBrightness(pair.dark), 80.0, 0.5);
}

TEST(CameraFramesTest, DaylightAveragingNeverContaminatedByAmbientOff) {
  // Daylight elevates OFF to 20, ON is 60.
  // When collecting 3 lit frames for averaging, only ON frames must be collected.
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kSunlightAmbient),
      makeSolidFrame(testSize(), kLitVal60),
      makeSolidFrame(testSize(), kSunlightAmbient),
      makeSolidFrame(testSize(), kLitVal60),
      makeSolidFrame(testSize(), kSunlightAmbient),
      makeSolidFrame(testSize(), kLitVal60)};

  SequenceReader reader(stream);
  auto collected = linuxcampam::collectLitFrames(
      [&](cv::Mat &f) { return reader(f); }, 3, kReads6, /*ambient_subtraction=*/false);

  ASSERT_EQ(collected.size(), 3);
  for (const auto &f : collected) {
    EXPECT_NEAR(meanBrightness(f), 60.0, 0.5);
  }
  cv::Mat avg = linuxcampam::averageFrames(collected);
  EXPECT_NEAR(meanBrightness(avg), 60.0, 0.5);
}

TEST(CameraFramesTest, DroppedFrameDoesNotBreakPhaseSeparation) {
  // A dropped frame flips phase order mid-stream:
  // [OFF=20, ON=60, ON=60 (drop caused repeat), OFF=20, ON=60, OFF=20]
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kSunlightAmbient),
      makeSolidFrame(testSize(), kLitVal60),
      makeSolidFrame(testSize(), kLitVal60),
      makeSolidFrame(testSize(), kSunlightAmbient),
      makeSolidFrame(testSize(), kLitVal60),
      makeSolidFrame(testSize(), kSunlightAmbient)};

  SequenceReader reader(stream);
  auto pair = linuxcampam::selectBestLitPair(
      [&](cv::Mat &f) { return reader(f); }, kReads6);

  ASSERT_FALSE(pair.lit.empty());
  ASSERT_TRUE(pair.has_dark);
  EXPECT_NEAR(meanBrightness(pair.lit), 60.0, 0.5);
  EXPECT_NEAR(meanBrightness(pair.dark), 20.0, 0.5);
}

TEST(CameraFramesTest, MultilevelBurstsAreClassifiedAmbiguousAndFailClosed) {
  // Multi-level burst without two compact clusters (OFF=80, ON in [100, 160, 220])
  // High spread across candidates lacks coherent bimodal interpretation.
  // Must be classified as Ambiguous and fail closed (empty lit selection/averaging).
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kLitVal80),
      makeSolidFrame(testSize(), kLitVal100),
      makeSolidFrame(testSize(), kLitVal80),
      makeSolidFrame(testSize(), kLitVal160),
      makeSolidFrame(testSize(), kLitVal80),
      makeSolidFrame(testSize(), kLitVal220)};

  SequenceReader reader(stream);
  auto pair = linuxcampam::selectBestLitPair(
      [&](cv::Mat &f) { return reader(f); }, kReads6);
  EXPECT_TRUE(pair.lit.empty());

  SequenceReader reader2(stream);
  auto collected = linuxcampam::collectLitFrames(
      [&](cv::Mat &f) { return reader2(f); }, 3, kReads6, false);
  EXPECT_TRUE(collected.empty());
}

TEST(CameraFramesTest, CompactStrobeSeparationWithTolerantSpread) {
  // Strobe burst with slight pulse variance within tolerance (spread <= 4.0):
  // OFF in [80, 81], ON in [120, 122]
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kLitVal80),
      makeSolidFrame(testSize(), kLitVal120),
      makeSolidFrame(testSize(), kLitVal81),
      makeSolidFrame(testSize(), kLitVal122),
      makeSolidFrame(testSize(), kLitVal80),
      makeSolidFrame(testSize(), kLitVal121)};

  SequenceReader reader(stream);
  auto pair = linuxcampam::selectBestLitPair(
      [&](cv::Mat &f) { return reader(f); }, kReads6);
  ASSERT_FALSE(pair.lit.empty());
  EXPECT_GE(meanBrightness(pair.lit), 119.5);
  ASSERT_TRUE(pair.has_dark);
  EXPECT_NEAR(meanBrightness(pair.dark), 80.5, 1.0);
}

TEST(CameraFramesTest, WeakSeparationIsClassifiedAmbiguousAndFailsClosed) {
  // Weak separation: OFF=80 / ON=86 (delta is 6.0, below kMinStrobeDelta=8.0)
  // Must be classified as Ambiguous and fail closed (empty lit frame)
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kLitVal80),
      makeSolidFrame(testSize(), kLitVal86),
      makeSolidFrame(testSize(), kLitVal80),
      makeSolidFrame(testSize(), kLitVal86),
      makeSolidFrame(testSize(), kLitVal80),
      makeSolidFrame(testSize(), kLitVal86)};

  SequenceReader reader(stream);
  cv::Mat best = linuxcampam::selectBestLitFrame(
      [&](cv::Mat &f) { return reader(f); }, kReads6);
  EXPECT_TRUE(best.empty());

  SequenceReader reader2(stream);
  auto collected = linuxcampam::collectLitFrames(
      [&](cv::Mat &f) { return reader2(f); }, 3, kReads6, false);
  EXPECT_TRUE(collected.empty());
}

TEST(CameraFramesTest, StableContinuousIlluminationAcceptsAllFrames) {
  // Steady illumination with minimal sensor noise (spread <= 4.0)
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kLitVal60),
      makeSolidFrame(testSize(), kLitVal61),
      makeSolidFrame(testSize(), kLitVal60),
      makeSolidFrame(testSize(), kLitVal61),
      makeSolidFrame(testSize(), kLitVal60)};

  SequenceReader reader(stream);
  cv::Mat best = linuxcampam::selectBestLitFrame(
      [&](cv::Mat &f) { return reader(f); }, kReads5);
  ASSERT_FALSE(best.empty());
  EXPECT_NEAR(meanBrightness(best), 60.5, 1.0);
}

TEST(CameraFramesTest, MonotonicExposureRampIsClassifiedAmbiguous) {
  // Ramping brightness from auto-exposure settling (50 to 100)
  // Lacks a distinct two-cluster strobe pattern -> must be ambiguous and fail closed
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kLitVal50),
      makeSolidFrame(testSize(), kLitVal60),
      makeSolidFrame(testSize(), kLitVal70),
      makeSolidFrame(testSize(), kLitVal80),
      makeSolidFrame(testSize(), kLitVal90),
      makeSolidFrame(testSize(), kLitVal100)};

  SequenceReader reader(stream);
  cv::Mat best = linuxcampam::selectBestLitFrame(
      [&](cv::Mat &f) { return reader(f); }, kReads6);
  EXPECT_TRUE(best.empty());
}

TEST(CameraFramesTest, StrobePhaseSelectionStartingOnLit) {
  // Strobe burst starting on lit frame (ON first, then OFF)
  std::vector<cv::Mat> stream = {
      makeSolidFrame(testSize(), kLitVal60),
      makeSolidFrame(testSize(), kSunlightAmbient),
      makeSolidFrame(testSize(), kLitVal60),
      makeSolidFrame(testSize(), kSunlightAmbient),
      makeSolidFrame(testSize(), kLitVal60),
      makeSolidFrame(testSize(), kSunlightAmbient)};

  SequenceReader reader(stream);
  auto pair = linuxcampam::selectBestLitPair(
      [&](cv::Mat &f) { return reader(f); }, kReads6);
  ASSERT_FALSE(pair.lit.empty());
  EXPECT_NEAR(meanBrightness(pair.lit), 60.0, 0.5);
  ASSERT_TRUE(pair.has_dark);
  EXPECT_NEAR(meanBrightness(pair.dark), 20.0, 0.5);
}

TEST(CameraFramesTest, VaryingAmbientMustNotWinSingleFrameSelection) {
  // Review regression: OFF=20,20,40; ON=100. Either select ON or conservatively reject.
  auto reader = [](const std::vector<int> &values) {
    return [vals = values, index = std::size_t{0}](cv::Mat &out) mutable {
      out.release();
      if (index == vals.size()) return false;
      const int val = vals.at(index++);
      if (val < 0) return false;
      out = makeSolidFrame(microSize(), static_cast<uint8_t>(val));
      return true;
    };
  };

  const auto frame = linuxcampam::selectBestLitFrame(
      reader({20, 100, 20, 100, 40, 100}));
  EXPECT_TRUE(frame.empty() || meanBrightness(frame) == 100.0);
}

TEST(CameraFramesTest, VaryingAmbientMustNotContaminateAverage) {
  auto reader = [](const std::vector<int> &values) {
    return [vals = values, index = std::size_t{0}](cv::Mat &out) mutable {
      out.release();
      if (index == vals.size()) return false;
      const int val = vals.at(index++);
      if (val < 0) return false;
      out = makeSolidFrame(microSize(), static_cast<uint8_t>(val));
      return true;
    };
  };

  const auto frames = linuxcampam::collectLitFrames(
      reader({20, 100, 20, 100, 40, 100,
              20, 100, 20, 100, 40, 100}), kReads5);
  for (const auto &frame : frames) {
    EXPECT_DOUBLE_EQ(meanBrightness(frame), 100.0);
  }
}

TEST(CameraFramesTest, FailedReadsMustNotProveContinuousIllumination) {
  auto reader = [](const std::vector<int> &values) {
    return [vals = values, index = std::size_t{0}](cv::Mat &out) mutable {
      out.release();
      if (index == vals.size()) return false;
      const int val = vals.at(index++);
      if (val < 0) return false;
      out = makeSolidFrame(microSize(), static_cast<uint8_t>(val));
      return true;
    };
  };

  const auto frame = linuxcampam::selectBestLitFrame(
      reader({80, -1, -1, -1, -1, -1}));
  EXPECT_TRUE(frame.empty());
}

TEST(CameraFramesTest, MissingOnPhaseMustNotBecomeAllOffAverage) {
  auto reader = [](const std::vector<int> &values) {
    return [vals = values, index = std::size_t{0}](cv::Mat &out) mutable {
      out.release();
      if (index == vals.size()) return false;
      const int val = vals.at(index++);
      if (val < 0) return false;
      out = makeSolidFrame(microSize(), static_cast<uint8_t>(val));
      return true;
    };
  };

  const auto frames = linuxcampam::collectLitFrames(
      reader({80, -1, 80, -1, 80, -1,
              80, -1, 80, -1, 80, -1}), kReads5);
  EXPECT_TRUE(frames.empty());
}
