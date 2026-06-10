#include "../src/parsers/Ite8353Parser.hpp"
#include <gtest/gtest.h>
#include <array>

constexpr size_t EXPECTED_PAYLOAD_SIZE = 12;
constexpr size_t SHORT_PAYLOAD_SIZE = 11;
constexpr uint8_t TEST_CONFIDENCE = 85;

TEST(Ite8353ParserTest, ParseValidPayload) {
  Ite8353Parser parser;
  std::array<uint8_t, EXPECTED_PAYLOAD_SIZE> buffer = {0x02, 0, 0, 0, 0, 0, 0, TEST_CONFIDENCE, 0, 0, 0, 1}; // Magic 0x02, Presence 1
  auto result = parser.parse_payload(buffer.data(), buffer.size());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->confidence_cm, TEST_CONFIDENCE);
  EXPECT_TRUE(result->human_present);
}

TEST(Ite8353ParserTest, ParseInvalidMagicByte) {
  Ite8353Parser parser;
  std::array<uint8_t, EXPECTED_PAYLOAD_SIZE> buffer = {0x03, 0, 0, 0, 0, 0, 0, TEST_CONFIDENCE, 0, 0, 0, 1}; // Magic 0x03 (invalid)
  auto result = parser.parse_payload(buffer.data(), buffer.size());
  EXPECT_FALSE(result.has_value());
}

TEST(Ite8353ParserTest, ParseTooSmallBuffer) {
  Ite8353Parser parser;
  std::array<uint8_t, SHORT_PAYLOAD_SIZE> buffer = {0x02, 0, 0, 0, 0, 0, 0, TEST_CONFIDENCE, 0, 0, 0}; // Size 11
  auto result = parser.parse_payload(buffer.data(), buffer.size());
  EXPECT_FALSE(result.has_value());
}

TEST(Ite8353ParserTest, NullBuffer) {
  Ite8353Parser parser;
  auto result = parser.parse_payload(nullptr, EXPECTED_PAYLOAD_SIZE);
  EXPECT_FALSE(result.has_value());
}
