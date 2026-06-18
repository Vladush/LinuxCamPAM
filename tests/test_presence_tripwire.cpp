#include "../src/PresenceTripwire.hpp"
#include "../src/SensorFactory.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <gtest/gtest.h>

// --- Mocks ---

class MockSensorParser final : public SensorParser {
  SensorState state_;
public:
  explicit MockSensorParser(SensorState s) : state_(s) {}
  [[nodiscard]] std::optional<SensorState> parse_payload(const uint8_t*, size_t) noexcept override {
    return state_;
  }
};

constexpr int DEFAULT_MOCK_CONFIDENCE_CM = 100;

// Mock factory returning fixed state
class ParsingFactory : public ISensorFactory {
  SensorState state_;
public:
  explicit ParsingFactory(SensorState s = {DEFAULT_MOCK_CONFIDENCE_CM, true}) : state_(s) {}
  [[nodiscard]] std::unique_ptr<SensorParser> create(std::string_view) const override {
    return std::make_unique<MockSensorParser>(state_);
  }
};

// Simulates unknown hardware ID
class NullSensorFactory : public ISensorFactory {
public:
  [[nodiscard]] std::unique_ptr<SensorParser> create(std::string_view) const override {
    return nullptr;
  }
};

// Mock HidOps (no-op/success)
static PresenceTripwireHidOps makePassthroughOps() {
  PresenceTripwireHidOps ops;
  ops.init       = [] { return 0; };
  // Hack: Cast sentinel to opaque hid_device* to satisfy non-null checks
  ops.open_path  = [](const char*) -> hid_device* {
    static char sentinel = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<hid_device*>(&sentinel);
  };
  ops.read_timeout = [](hid_device*, uint8_t*, size_t, int) { return 0; };
  ops.close_fn   = [](hid_device*) {};
  ops.exit_fn    = [] {};
  return ops;
}

// --- Safety tests ---

class PresenceTripwireTest : public ::testing::Test {};

TEST_F(PresenceTripwireTest, StartFailsWithInvalidHidrawNode) {
  NullSensorFactory factory;
  PresenceTripwire tripwire(factory);
  HardwareId hw_id("ValidId");
  bool result = tripwire.start("/dev/invalid_hidraw_node", hw_id, [](bool, int){});
  EXPECT_FALSE(result);
}

TEST_F(PresenceTripwireTest, StopWithoutStartIsSafe) {
  NullSensorFactory factory;
  PresenceTripwire tripwire(factory);
  tripwire.stop();
}

TEST_F(PresenceTripwireTest, SafeDestruction) {
  {
    NullSensorFactory factory;
    PresenceTripwire tripwire(factory);
  }
}

// --- HidOps tests ---

TEST_F(PresenceTripwireTest, StartReturnsTrueWithWorkingHid) {
  ParsingFactory factory;
  PresenceTripwire tripwire(factory, makePassthroughOps());
  EXPECT_TRUE(tripwire.start("/dev/fake", HardwareId("ValidId"), [](bool, int){}));
  tripwire.stop();
}

TEST_F(PresenceTripwireTest, HidInitFailurePreventsStart) {
  ParsingFactory factory;
  auto ops = makePassthroughOps();
  ops.init = [] { return -1; };

  PresenceTripwire tripwire(factory, std::move(ops));
  EXPECT_FALSE(tripwire.start("/dev/fake", HardwareId("ValidId"), [](bool, int){}));
}

TEST_F(PresenceTripwireTest, HidOpenFailurePreventsStart) {
  ParsingFactory factory;
  auto ops = makePassthroughOps();
  ops.open_path = [](const char*) -> hid_device* { return nullptr; };

  PresenceTripwire tripwire(factory, std::move(ops));
  EXPECT_FALSE(tripwire.start("/dev/fake", HardwareId("ValidId"), [](bool, int){}));
}

TEST_F(PresenceTripwireTest, PollLoopFiresCallbackOnPresence) {
  constexpr int EXPECTED_CONFIDENCE = 100;
  ParsingFactory factory({EXPECTED_CONFIDENCE, true});

  std::atomic<bool> data_sent{false};
  auto ops = makePassthroughOps();
  ops.read_timeout = [&data_sent](hid_device*, uint8_t* buf, size_t, int) -> int {
    if (data_sent.load(std::memory_order_relaxed)) return 0; // park after first read
    data_sent.store(true, std::memory_order_relaxed);
    buf[0] = 0x01;
    return 1;
  };

  std::promise<std::pair<bool, int>> promise;
  auto future = promise.get_future();
  std::once_flag once;

  PresenceTripwire tripwire(factory, std::move(ops));
  ASSERT_TRUE(tripwire.start("/dev/fake", HardwareId("ValidId"),
    [&](bool present, int conf) {
      std::call_once(once, [&] { promise.set_value({present, conf}); });
    }));

  ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  auto [present, conf] = future.get();
  EXPECT_TRUE(present);
  EXPECT_EQ(conf, EXPECTED_CONFIDENCE);
  tripwire.stop();
}

TEST_F(PresenceTripwireTest, PollLoopAbortsOnHidError) {
  ParsingFactory factory;
  std::atomic<bool> callback_called{false};

  auto ops = makePassthroughOps();
  ops.read_timeout = [](hid_device*, uint8_t*, size_t, int) -> int { return -1; };

  // Use scope to ensure thread joins
  {
    PresenceTripwire tripwire(factory, std::move(ops));
    ASSERT_TRUE(tripwire.start("/dev/fake", HardwareId("ValidId"),
      [&](bool, int) { callback_called = true; }));
  }

  EXPECT_FALSE(callback_called);
}
