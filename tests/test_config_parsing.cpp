#include "../src/service/config.hpp"

#include <gtest/gtest.h>
#include <sstream>

using namespace linuxcampam;

class ConfigParsingTest : public ::testing::Test {
public: // NOLINT(misc-non-private-member-variables-in-classes)
  Configuration config;
  
  void loadConfig(std::string_view ini_content) {
    std::stringstream ss(std::string{ini_content});
    EXPECT_TRUE(config.load(ss, nullptr));
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
  loadConfig(R"(
[Auth]
threshold=bad_float
timeout_ms=not_an_int
max_embeddings=-invalid
  )");
  
  // Should retain defaults since parsing failed
  EXPECT_FLOAT_EQ(config.threshold, Configuration::DEFAULT_THRESHOLD);
  EXPECT_EQ(config.timeout_ms, Configuration::DEFAULT_TIMEOUT_MS);
  EXPECT_EQ(config.max_embeddings, Configuration::DEFAULT_MAX_EMBEDDINGS);
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
  EXPECT_EQ(config.provider_priority[0], "TensorRT");
  EXPECT_EQ(config.provider_priority[1], "CUDA");
  EXPECT_EQ(config.provider_priority[2], "CPU");
}

TEST_F(ConfigParsingTest, EmptyProviderPriorityUsesDefault) {
  loadConfig(R"(
[Hardware]
provider_priority=
  )");
  
  ASSERT_EQ(config.provider_priority.size(), 2);
  EXPECT_EQ(config.provider_priority[0], "OpenCL");
  EXPECT_EQ(config.provider_priority[1], "CPU");
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
  
  EXPECT_EQ(config.camera_defs[0].id, "cam1");
  EXPECT_EQ(config.camera_defs[0].path, "/dev/video1");
  EXPECT_EQ(config.camera_defs[0].type, "rgb");
  EXPECT_TRUE(config.camera_defs[0].mandatory);
  EXPECT_EQ(config.camera_defs[0].min_brightness, 10);
  
  EXPECT_EQ(config.camera_defs[1].id, "cam2");
  EXPECT_EQ(config.camera_defs[1].path, "/dev/video2");
  EXPECT_EQ(config.camera_defs[1].type, "ir");
  EXPECT_FALSE(config.camera_defs[1].mandatory);
  EXPECT_EQ(config.camera_defs[1].enroll_hdr, "true");
  EXPECT_EQ(config.camera_defs[1].enroll_averaging, "true");
  EXPECT_EQ(config.camera_defs[1].enroll_average_frames, 5);
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
