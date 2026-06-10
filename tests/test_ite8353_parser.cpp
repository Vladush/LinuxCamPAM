#include "../src/parsers/Ite8353Parser.hpp"
#include <gtest/gtest.h>

TEST(Ite8353ParserTest, ParseValidPayload) {
  Ite8353Parser parser;
  uint8_t buffer[12] = {0x02, 0, 0, 0, 0, 0, 0, 85, 0, 0, 0, 1}; // Magic 0x02, Confidence 85, Presence 1
  auto result = parser.parse_payload(buffer, sizeof(buffer));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->confidence_cm, 85);
  EXPECT_TRUE(result->human_present);
}

TEST(Ite8353ParserTest, ParseInvalidMagicByte) {
  Ite8353Parser parser;
  uint8_t buffer[12] = {0x03, 0, 0, 0, 0, 0, 0, 85, 0, 0, 0, 1}; // Magic 0x03 (invalid)
  auto result = parser.parse_payload(buffer, sizeof(buffer));
  EXPECT_FALSE(result.has_value());
}

TEST(Ite8353ParserTest, ParseTooSmallBuffer) {
  Ite8353Parser parser;
  uint8_t buffer[11] = {0x02, 0, 0, 0, 0, 0, 0, 85, 0, 0, 0}; // Size 11
  auto result = parser.parse_payload(buffer, sizeof(buffer));
  EXPECT_FALSE(result.has_value());
}

TEST(Ite8353ParserTest, NullBuffer) {
  Ite8353Parser parser;
  auto result = parser.parse_payload(nullptr, 12);
  EXPECT_FALSE(result.has_value());
}
