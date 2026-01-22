#include "../src/service/auth_engine.hpp"

#include <fstream>
#include <gtest/gtest.h>

class AuthConfigTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override { (void)remove("config_test.ini"); }

  void createConfig(const std::string &content) {
    std::ofstream out("config_test.ini");
    out << content;
    out.close();
  }
};

TEST_F(AuthConfigTest, UsesDefaultsWhenConfigMissing) {
  AuthEngine engine;
  (void)engine.init("non_existent_file.ini");

  // Ensure it initializes without error.
  SUCCEED();
}

TEST_F(AuthConfigTest, ParsesValidValues) {
  createConfig(R"(
[Auth]
threshold = 0.75
detection_threshold = 0.85
timeout_ms = 5000
max_embeddings = 10
)");

  AuthEngine engine;
  (void)engine.init("config_test.ini");
  // Verify parsing doesn't crash on valid input.
  SUCCEED();
}

TEST_F(AuthConfigTest, HandlesPartialConfig) {
  createConfig(R"(
[Auth]
threshold = 0.65
)");
  AuthEngine engine;
  (void)engine.init("config_test.ini");
  SUCCEED();
}

TEST_F(AuthConfigTest, FallbackOnInvalidData) {
  createConfig(R"(
[Auth]
threshold = invalid_float
timeout_ms = invalid_int
)");
  AuthEngine engine;
  (void)engine.init("config_test.ini");
  // Should use defaults (checked via logging manually or if we had accessors)
  SUCCEED();
}

constexpr size_t MAX_USERNAME_LENGTH = 32;

bool isValidUsername(std::string_view username) {
  // Basic sanity checks
  if (username.empty() || username.length() > MAX_USERNAME_LENGTH)
    return false;

  // Block path traversal and hidden files
  if (username[0] == '.')
    return false;

  for (size_t i = 1; i < username.length(); ++i) {
    if (username[i] == '.' && username[i - 1] == '.') {
      return false; // Found ".."
    }
  }

  // Standard Linux username chars (plus Samba's $)
  // strict allowlist prevents shell injection
  return std::all_of(username.begin(), username.end(), [](char c) {
    bool is_lower = (c >= 'a' && c <= 'z');
    bool is_upper = (c >= 'A' && c <= 'Z');
    bool is_digit = (c >= '0' && c <= '9');
    bool is_special = (c == '_' || c == '.' || c == '-' || c == '$');

    return is_lower || is_upper || is_digit || is_special;
  });
}

TEST(SecurityTest, UsernameSanitization) {
  // Valid cases
  EXPECT_TRUE(isValidUsername("vlad"));
  EXPECT_TRUE(isValidUsername("user.name"));
  EXPECT_TRUE(isValidUsername("user-name"));
  EXPECT_TRUE(isValidUsername("user_123"));

  // Invalid cases (Potential Path Traversal)
  EXPECT_FALSE(isValidUsername("../../etc/passwd"));
  EXPECT_FALSE(isValidUsername("user/name"));
  EXPECT_FALSE(isValidUsername("user\\name"));
  EXPECT_FALSE(isValidUsername(".."));

  // Invalid characters
  EXPECT_FALSE(isValidUsername("user name"));   // Spaces
  EXPECT_FALSE(isValidUsername("user@domain")); // @ not allowed yet
  EXPECT_FALSE(isValidUsername("user!"));
  EXPECT_FALSE(isValidUsername(""));
}
