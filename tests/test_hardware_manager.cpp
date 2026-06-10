#include "../src/HardwareManager.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

class HardwareManagerTest : public ::testing::Test {
protected:
  fs::path temp_dir;
  fs::path sys_i2c;
  fs::path sys_hid;
  fs::path dev_dir;

  void SetUp() override {
    std::error_code ec;
    temp_dir = fs::path("/tmp/linuxcampam_hw_test");
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir / "sys/bus/i2c/devices");
    fs::create_directories(temp_dir / "sys/bus/hid/devices");
    fs::create_directories(temp_dir / "dev");
    
    sys_i2c = temp_dir / "sys/bus/i2c/devices";
    sys_hid = temp_dir / "sys/bus/hid/devices";
    dev_dir = temp_dir / "dev";
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
  }
};

TEST_F(HardwareManagerTest, ConstructorAndDestructor) {
  HardwareManager hm("test_addr", sys_i2c, sys_hid, dev_dir);
  // Destructor should not throw
}

TEST_F(HardwareManagerTest, SeizeSensorFailsIfPathMissing) {
  HardwareManager hm("missing_addr", sys_i2c, sys_hid, dev_dir);
  EXPECT_FALSE(hm.seize_sensor());
}

TEST_F(HardwareManagerTest, SeizeSensorSucceedsWithMockDirs) {
  std::error_code ec;
  fs::path device_dir = sys_i2c / "test_addr";
  fs::create_directories(device_dir / "power");
  
  std::ofstream(device_dir / "power" / "wakeup") << "enabled";

  fs::create_directories(device_dir / "0018:ABCD:1234.0001");

  HardwareManager hm("test_addr", sys_i2c, sys_hid, dev_dir);
  EXPECT_TRUE(hm.seize_sensor());
}

TEST_F(HardwareManagerTest, GetHidrawNodeReturnsNoneIfMissing) {
  HardwareManager hm("test_addr", sys_i2c, sys_hid, dev_dir);
  EXPECT_FALSE(hm.get_hidraw_node().has_value());
}

TEST_F(HardwareManagerTest, GetHidrawNodeSucceeds) {
  std::error_code ec;
  fs::path device_dir = sys_i2c / "test_addr";
  fs::create_directories(device_dir / "power");
  std::ofstream(device_dir / "power" / "wakeup") << "enabled";
  fs::create_directories(device_dir / "0018:ABCD:1234.0001");

  HardwareManager hm("test_addr", sys_i2c, sys_hid, dev_dir);
  EXPECT_TRUE(hm.seize_sensor());

  fs::path hid_device_dir = sys_hid / "0018:ABCD:1234.0001";
  fs::create_directories(hid_device_dir / "hidraw" / "hidraw0");

  auto node = hm.get_hidraw_node();
  ASSERT_TRUE(node.has_value());
  EXPECT_EQ(node.value(), (dev_dir / "hidraw0").string());
}
