#include "../src/HardwareManager.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

class HardwareManagerTest : public ::testing::Test {
private:
  fs::path temp_dir_;
  fs::path sys_i2c_;
  fs::path sys_hid_;
  fs::path dev_dir_;

protected:
  fs::path get_temp_dir() const { return temp_dir_; }
  fs::path get_sys_i2c() const { return sys_i2c_; }
  fs::path get_sys_hid() const { return sys_hid_; }
  fs::path get_dev_dir() const { return dev_dir_; }

  void SetUp() override {
    std::error_code ec;
    temp_dir_ = fs::path("/tmp/linuxcampam_hw_test");
    fs::remove_all(temp_dir_, ec);
    fs::create_directories(temp_dir_ / "sys/bus/i2c/devices");
    fs::create_directories(temp_dir_ / "sys/bus/hid/devices");
    fs::create_directories(temp_dir_ / "dev");
    
    sys_i2c_ = temp_dir_ / "sys/bus/i2c/devices";
    sys_hid_ = temp_dir_ / "sys/bus/hid/devices";
    dev_dir_ = temp_dir_ / "dev";
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(temp_dir_, ec);
  }
};

TEST_F(HardwareManagerTest, ConstructorAndDestructor) {
  HardwareManager hm("test_addr", get_sys_i2c(), get_sys_hid(), get_dev_dir());
  // Destructor should not throw
}

TEST_F(HardwareManagerTest, SeizeSensorFailsIfPathMissing) {
  HardwareManager hm("missing_addr", get_sys_i2c(), get_sys_hid(), get_dev_dir());
  EXPECT_FALSE(hm.seize_sensor());
}

TEST_F(HardwareManagerTest, SeizeSensorSucceedsWithMockDirs) {
  fs::path device_dir = get_sys_i2c() / "test_addr";
  fs::create_directories(device_dir / "power");
  
  std::ofstream(device_dir / "power" / "wakeup") << "enabled";

  fs::create_directories(device_dir / "0018:ABCD:1234.0001");

  HardwareManager hm("test_addr", get_sys_i2c(), get_sys_hid(), get_dev_dir());
  EXPECT_TRUE(hm.seize_sensor());
}

TEST_F(HardwareManagerTest, GetHidrawNodeReturnsNoneIfMissing) {
  HardwareManager hm("test_addr", get_sys_i2c(), get_sys_hid(), get_dev_dir());
  EXPECT_FALSE(hm.get_hidraw_node().has_value());
}

TEST_F(HardwareManagerTest, GetHidrawNodeSucceeds) {
  fs::path device_dir = get_sys_i2c() / "test_addr";
  fs::create_directories(device_dir / "power");
  std::ofstream(device_dir / "power" / "wakeup") << "enabled";
  fs::create_directories(device_dir / "0018:ABCD:1234.0001");

  HardwareManager hm("test_addr", get_sys_i2c(), get_sys_hid(), get_dev_dir());
  EXPECT_TRUE(hm.seize_sensor());

  fs::path hid_device_dir = get_sys_hid() / "0018:ABCD:1234.0001";
  fs::create_directories(hid_device_dir / "hidraw" / "hidraw0");

  auto node = hm.get_hidraw_node();
  ASSERT_TRUE(node.has_value());
  EXPECT_EQ(node.value(), (get_dev_dir() / "hidraw0").string());
}
