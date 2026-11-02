#include "../src/service/config.hpp"

#include <gtest/gtest.h>
#include <sstream>

using namespace linuxcampam;

class MockEmptyBackend : public linuxcampam::ICameraBackend {
public:
  [[nodiscard]] std::vector<std::string> getDevicePaths() const override {
    return {};
  }
  [[nodiscard]] bool isVideoCaptureDevice(std::string_view) const override {
    return false;
  }
  [[nodiscard]] std::vector<uint32_t> getPixelFormats(std::string_view) const override {
    return {};
  }
};

class ConfigParsingTest : public ::testing::Test {
public: // NOLINT(misc-non-private-member-variables-in-classes)
  Configuration config;
  MockEmptyBackend mock_backend;
  
  void loadConfig(std::string_view ini_content) {
    std::stringstream ss(std::string{ini_content});
    EXPECT_TRUE(config.load(ss, &mock_backend));
  }
  bool loadStream(std::istream &ss) {
    return config.load(ss, &mock_backend);
  }
};

TEST_F(ConfigParsingTest, FallbackFromAuthToGeneral) {
  loadConfig(R"(
[General]
threshold=0.85
detection_threshold=0.95
timeout_ms=5000
auth_method=strict
max_embeddings=10
  )");
  
  EXPECT_FLOAT_EQ(config.threshold, 0.85f);
  EXPECT_FLOAT_EQ(config.detection_threshold, 0.95f);
  EXPECT_EQ(config.timeout_ms, 5000);
  EXPECT_EQ(config.policy, Configuration::AuthPolicy::STRICT_ALL);
  EXPECT_EQ(config.max_embeddings, 10);
}

TEST_F(ConfigParsingTest, AuthOverridesGeneral) {
  loadConfig(R"(
[General]
threshold=0.5
detection_threshold=0.5
timeout_ms=1000
auth_method=lenient
max_embeddings=2

[Auth]
threshold=0.99
detection_threshold=0.99
timeout_ms=8000
policy=strict
max_embeddings=20
  )");
  
  EXPECT_FLOAT_EQ(config.threshold, 0.99f);
  EXPECT_FLOAT_EQ(config.detection_threshold, 0.99f);
  EXPECT_EQ(config.timeout_ms, 8000);
  EXPECT_EQ(config.policy, Configuration::AuthPolicy::STRICT_ALL);
  EXPECT_EQ(config.max_embeddings, 20);
}

TEST_F(ConfigParsingTest, HandlesMalformedNumbers) {
  std::stringstream ss(std::string{R"(
[Auth]
threshold=bad_float
timeout_ms=not_an_int
max_embeddings=-invalid
  )"});
  
  // Should fail since parsing failed
  EXPECT_FALSE(config.load(ss, nullptr));
}

TEST_F(ConfigParsingTest, ProximitySensorSettings) {
  loadConfig(R"(
[Hardware]
proximity_sensor=enabled
proximity_sensor_id=TEST_ID
proximity_enforce=true

[Proximity]
wake_enabled=false
always_wake_on_presence_detected=false
wake_confidence_threshold=80
lock_enabled=true
lock_confidence_threshold=10
lock_timeout_seconds=5
lock_command=custom_lock
  )");
  
  EXPECT_EQ(config.proximity_sensor, Configuration::ProximitySensorMode::ENABLED);
  EXPECT_EQ(config.proximity_sensor_id, "TEST_ID");
  EXPECT_TRUE(config.proximity_enforce);
  
  EXPECT_FALSE(config.wake_enabled);
  EXPECT_FALSE(config.always_wake_on_presence_detected);
  EXPECT_EQ(config.wake_confidence_threshold, 80);
  
  EXPECT_TRUE(config.lock_enabled);
  EXPECT_EQ(config.lock_confidence_threshold, 10);
  EXPECT_EQ(config.lock_timeout_seconds, 5);
  EXPECT_EQ(config.lock_command, "custom_lock");
}

TEST_F(ConfigParsingTest, ProviderPriorityParsing) {
  loadConfig(R"(
[Hardware]
provider_priority=TensorRT, CUDA, CPU
  )");
  
  ASSERT_EQ(config.provider_priority.size(), 3);
  EXPECT_EQ(config.provider_priority.at(0), "TensorRT");
  EXPECT_EQ(config.provider_priority.at(1), "CUDA");
  EXPECT_EQ(config.provider_priority.at(2), "CPU");
}

TEST_F(ConfigParsingTest, EmptyProviderPriorityUsesDefault) {
  loadConfig(R"(
[Hardware]
provider_priority=
  )");
  
  ASSERT_EQ(config.provider_priority.size(), 2);
  EXPECT_EQ(config.provider_priority.at(0), "OpenCL");
  EXPECT_EQ(config.provider_priority.at(1), "CPU");
}

TEST_F(ConfigParsingTest, ExplicitCameraDefinitions) {
  loadConfig(R"(
[Cameras]
names=cam1, cam2

[Camera.cam1]
path=/dev/video1
type=rgb
mandatory=true
min_brightness=10

[Camera.cam2]
path=/dev/video2
type=ir
enroll_hdr=true
enroll_averaging=true
enroll_average_frames=5
  )");
  
  ASSERT_EQ(config.camera_defs.size(), 2);
  
  EXPECT_EQ(config.camera_defs.at(0).id, "cam1");
  EXPECT_EQ(config.camera_defs.at(0).path, "/dev/video1");
  EXPECT_EQ(config.camera_defs.at(0).type, "rgb");
  EXPECT_TRUE(config.camera_defs.at(0).mandatory);
  EXPECT_EQ(config.camera_defs.at(0).min_brightness, 10);
  
  EXPECT_EQ(config.camera_defs.at(1).id, "cam2");
  EXPECT_EQ(config.camera_defs.at(1).path, "/dev/video2");
  EXPECT_EQ(config.camera_defs.at(1).type, "ir");
  EXPECT_FALSE(config.camera_defs.at(1).mandatory);
  EXPECT_EQ(config.camera_defs.at(1).enroll_hdr, "true");
  EXPECT_EQ(config.camera_defs.at(1).enroll_averaging, std::optional<bool>(true));
  EXPECT_EQ(config.camera_defs.at(1).enroll_average_frames, 5);
}

TEST_F(ConfigParsingTest, ToStringOutputContainsKeyElements) {
  loadConfig(R"(
[General]
log_level=debug
[Auth]
policy=lenient
[Hardware]
proximity_sensor=disabled
  )");
  
  std::string out = config.toString();
  EXPECT_NE(out.find("Log Level: debug"), std::string::npos);
  EXPECT_NE(out.find("Lenient (Any Camera Match)"), std::string::npos);
  EXPECT_NE(out.find("Proximity Sensor: disabled"), std::string::npos);
}

TEST_F(ConfigParsingTest, MinUidParsing) {
  loadConfig(R"(
[Security]
min_uid=2000
  )");
  EXPECT_EQ(config.min_uid, 2000);
}

TEST_F(ConfigParsingTest, NegativeMinUidFallback) {
  loadConfig(R"(
[Security]
min_uid=-5
  )");
  EXPECT_EQ(config.min_uid, DEFAULT_MIN_UID);
}

TEST_F(ConfigParsingTest, PerformanceSettings) {
  loadConfig(R"(
[Performance]
gpu_flush=off
gpu_throttle_ms=50
model_keep_alive_sec=120
  )");
  EXPECT_FALSE(config.gpu_flush);
  EXPECT_EQ(config.gpu_throttle_ms, 50);
  EXPECT_EQ(config.model_keep_alive_sec, 120);
}

TEST_F(ConfigParsingTest, RejectsMalformedSyntaxWithoutChangingActiveConfiguration) {
  loadConfig("[Hardware]\nproximity_enforce=true\n[Auth]\nthreshold=0.8\n");
  for (const std::string malformed : {"[Hardware\nproximity_enforce=false", "[ ]", "[Hardware]junk",
                                      "proximity_enforce true", "=false", "name=\"unclosed"}) {
    SCOPED_TRACE(malformed);
    std::istringstream input(malformed);
    EXPECT_FALSE(loadStream(input));
    EXPECT_TRUE(config.proximity_enforce);
    EXPECT_FLOAT_EQ(config.threshold, 0.8f);
  }
}

TEST_F(ConfigParsingTest, ReadFailurePreservesConfiguration) {
  constexpr float kCustomThreshold = 0.8F;
  config.threshold = kCustomThreshold;
  std::istringstream input;
  input.setstate(std::ios::badbit);
  EXPECT_FALSE(loadStream(input));
  EXPECT_FLOAT_EQ(config.threshold, kCustomThreshold);
}

TEST_F(ConfigParsingTest, LateValueErrorPreservesEntireConfiguration) {
  loadConfig("[Auth]\nthreshold=0.8\n[Hardware]\nproximity_enforce=true\n");
  std::istringstream input("[Auth]\nthreshold=0.2\n[Cameras]\nnames=x\n[Camera.x]\nmandatory=typo\n");
  EXPECT_FALSE(loadStream(input));
  EXPECT_FLOAT_EQ(config.threshold, 0.8f);
  EXPECT_TRUE(config.proximity_enforce);
}

TEST_F(ConfigParsingTest, ListsTrimBothSidesAndIgnoreEmptyItems) {
  loadConfig("[Hardware]\nprovider_priority= OpenCL , , CPU \n[Cameras]\nnames= a , , b \n");
  EXPECT_EQ(config.provider_priority, (std::vector<std::string>{"OpenCL", "CPU"}));
  ASSERT_EQ(config.camera_defs.size(), 2);
  EXPECT_EQ(config.camera_defs.at(0).id, "a");
  EXPECT_EQ(config.camera_defs.at(1).id, "b");
}

TEST_F(ConfigParsingTest, GpuFlushBooleanSpellings) {
  for (const std::string value : {"true", "on", "yes", "1", "false", "off", "no", "0"}) {
    loadConfig("[Performance]\ngpu_flush=" + value + "\n");
    EXPECT_EQ(config.gpu_flush, value == "true" || value == "on" || value == "yes" || value == "1");
  }
}

TEST_F(ConfigParsingTest, AuthenticationDeadlineMustBeBounded) {
  for (const std::string value : {"0", "-1", "60001", "2147483647"}) {
    std::istringstream input("[Auth]\ntimeout_ms=" + value);
    EXPECT_FALSE(loadStream(input));
  }
}

TEST_F(ConfigParsingTest, AverageFramesMustBeBounded) {
  for (const std::string value : {"0", "-1", "16", "1000"}) {
    std::istringstream input_enroll("[Capture]\nenroll_average_frames=" + value);
    EXPECT_FALSE(loadStream(input_enroll));

    std::istringstream input_verify("[Capture]\nverify_average_frames=" + value);
    EXPECT_FALSE(loadStream(input_verify));
  }

  std::istringstream valid("[Capture]\nenroll_average_frames=5\nverify_average_frames=3\n");
  EXPECT_TRUE(loadStream(valid));
  EXPECT_EQ(config.enroll_average_frames, 5);
  EXPECT_EQ(config.verify_average_frames, 3);
}

TEST_F(ConfigParsingTest, EnrollHdrStrictParsing) {
  loadConfig("[Capture]\nenroll_hdr=on\n");
  EXPECT_EQ(config.enroll_hdr, "on");

  loadConfig("[Capture]\nenroll_hdr=auto\n");
  EXPECT_EQ(config.enroll_hdr, "auto");

  loadConfig("[Capture]\nenroll_hdr=off\n");
  EXPECT_EQ(config.enroll_hdr, "off");

  loadConfig("[Capture]\nenroll_hdr=true\n");
  EXPECT_EQ(config.enroll_hdr, "on");

  loadConfig("[Capture]\nenroll_hdr=false\n");
  EXPECT_EQ(config.enroll_hdr, "off");

  std::istringstream invalid("[Capture]\nenroll_hdr=bananas\n");
  EXPECT_FALSE(loadStream(invalid));
}

TEST_F(ConfigParsingTest, CameraEnrollAveragingBooleanStrictParsing) {
  std::istringstream invalid(R"(
[Cameras]
names=ir
[Camera.ir]
path=/dev/video0
enroll_averaging=bananas
)");
  EXPECT_FALSE(loadStream(invalid));

  std::istringstream valid_on(R"(
[Cameras]
names=ir
[Camera.ir]
path=/dev/video0
enroll_averaging=on
)");
  EXPECT_TRUE(loadStream(valid_on));
  EXPECT_EQ(config.camera_defs.at(0).enroll_averaging, std::optional<bool>(true));

  std::istringstream valid_off(R"(
[Cameras]
names=ir
[Camera.ir]
path=/dev/video0
enroll_averaging=off
)");
  EXPECT_TRUE(loadStream(valid_off));
  EXPECT_EQ(config.camera_defs.at(0).enroll_averaging, std::optional<bool>(false));

  std::istringstream unset(R"(
[Cameras]
names=ir
[Camera.ir]
path=/dev/video0
)");
  EXPECT_TRUE(loadStream(unset));
  EXPECT_EQ(config.camera_defs.at(0).enroll_averaging, std::nullopt);
}

TEST_F(ConfigParsingTest, CameraEnrollAverageFramesBounded) {
  for (const std::string value : {"0", "-1", "16", "1000"}) {
    std::istringstream invalid(R"(
[Cameras]
names=ir
[Camera.ir]
path=/dev/video0
enroll_average_frames=)" + value + "\n");
    EXPECT_FALSE(loadStream(invalid));
  }

  std::istringstream valid(R"(
[Cameras]
names=ir
[Camera.ir]
path=/dev/video0
enroll_average_frames=5
)");
  EXPECT_TRUE(loadStream(valid));
  EXPECT_EQ(config.camera_defs.at(0).enroll_average_frames, std::optional<int>(5));

  std::istringstream unset(R"(
[Cameras]
names=ir
[Camera.ir]
path=/dev/video0
)");
  EXPECT_TRUE(loadStream(unset));
  EXPECT_EQ(config.camera_defs.at(0).enroll_average_frames, std::nullopt);
}

TEST_F(ConfigParsingTest, AmbientSubtractionDefaultsToFalse) {
  std::istringstream stream(R"(
[Cameras]
names=ir
[Camera.ir]
path=/dev/video0
)");
  EXPECT_TRUE(loadStream(stream));
  EXPECT_FALSE(config.ir_ambient_subtraction);
  EXPECT_EQ(config.camera_defs.at(0).ambient_subtraction, std::nullopt);
}

TEST_F(ConfigParsingTest, CameraAmbientSubtractionStrictParsing) {
  std::istringstream invalid(R"(
[Cameras]
names=ir
[Camera.ir]
path=/dev/video0
ambient_subtraction=invalid_val
)");
  EXPECT_FALSE(loadStream(invalid));

  std::istringstream valid_off(R"(
[Cameras]
names=ir
[Camera.ir]
path=/dev/video0
ambient_subtraction=off
)");
  EXPECT_TRUE(loadStream(valid_off));
  EXPECT_EQ(config.camera_defs.at(0).ambient_subtraction, std::optional<bool>(false));

  // Requests to enable ambient subtraction are explicitly rejected since feature is deferred
  std::istringstream rejected_on(R"(
[Cameras]
names=ir
[Camera.ir]
path=/dev/video0
ambient_subtraction=true
)");
  EXPECT_FALSE(loadStream(rejected_on));
}

TEST_F(ConfigParsingTest, AmbientSubtractionAliasesAndValidation) {
  // Canonical only
  std::istringstream canonical(R"(
[Cameras]
names=ir
[Camera.ir]
path=/dev/video0
[Capture]
ir_ambient_subtraction=false
)");
  EXPECT_TRUE(loadStream(canonical));
  EXPECT_FALSE(config.ir_ambient_subtraction);

  // Alias only
  std::istringstream alias(R"(
[Cameras]
names=ir
[Camera.ir]
path=/dev/video0
[Hardware]
ir_ambient_subtraction=false
)");
  EXPECT_TRUE(loadStream(alias));
  EXPECT_FALSE(config.ir_ambient_subtraction);

  // Equal duplicate
  std::istringstream duplicate(R"(
[Cameras]
names=ir
[Camera.ir]
path=/dev/video0
[Capture]
ir_ambient_subtraction=false
[Hardware]
ir_ambient_subtraction=false
)");
  EXPECT_TRUE(loadStream(duplicate));
  EXPECT_FALSE(config.ir_ambient_subtraction);

  // Malformed shadowed alias must NOT be ignored
  std::istringstream malformed_shadowed(R"(
[Cameras]
names=ir
[Camera.ir]
path=/dev/video0
[Capture]
ir_ambient_subtraction=false
[Hardware]
ir_ambient_subtraction=bananas
)");
  EXPECT_FALSE(loadStream(malformed_shadowed));

  // Explicit enable request must be rejected
  std::istringstream enable_req(R"(
[Cameras]
names=ir
[Camera.ir]
path=/dev/video0
[Capture]
ir_ambient_subtraction=true
)");
  EXPECT_FALSE(loadStream(enable_req));

  // Alias enable request must also be rejected
  std::istringstream alias_enable_req(R"(
[Cameras]
names=ir
[Camera.ir]
path=/dev/video0
[Hardware]
ir_ambient_subtraction=true
)");
  EXPECT_FALSE(loadStream(alias_enable_req));
}
