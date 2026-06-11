#include "../src/PresenceTripwire.hpp"
#include "../src/SensorFactory.hpp"
#include <gtest/gtest.h>

class MockSensorFactory : public ISensorFactory {
public:
  std::unique_ptr<SensorParser> create(std::string_view /*hardware_id*/) const override {
    return nullptr;
  }
};

class PresenceTripwireTest : public ::testing::Test {
};

TEST_F(PresenceTripwireTest, StartFailsWithInvalidHidrawNode) {
  MockSensorFactory factory;
  PresenceTripwire tripwire(factory);
  HardwareId hw_id("ValidId");
  bool result = tripwire.start("/dev/invalid_hidraw_node", hw_id, [](bool, int){});
  EXPECT_FALSE(result);
}

TEST_F(PresenceTripwireTest, StopWithoutStartIsSafe) {
  MockSensorFactory factory;
  PresenceTripwire tripwire(factory);
  tripwire.stop();
}

TEST_F(PresenceTripwireTest, SafeDestruction) {
  {
    MockSensorFactory factory;
    PresenceTripwire tripwire(factory);
  }
}
