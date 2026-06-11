#include "../src/SensorFactory.hpp"
#include <gtest/gtest.h>

TEST(SensorFactoryTest, CreateKnownParser) {
  SensorFactory factory;
  auto parser = factory.create("ITE8353");
  ASSERT_NE(parser, nullptr);
}

TEST(SensorFactoryTest, CreateUnknownParserReturnsNull) {
  SensorFactory factory;
  auto parser = factory.create("UNKNOWN_SENSOR");
  EXPECT_EQ(parser, nullptr);
}
