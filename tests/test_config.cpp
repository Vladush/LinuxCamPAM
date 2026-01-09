#include <gtest/gtest.h>
#include <string>
#include <string_view>

bool isValidUsername(std::string_view username) {
  // 1. Basic sanity checks
  if (username.empty() || username.length() > 32)
    return false;

  // 2. Prevent Path Traversal (CRITICAL)
  // Rejects ".." and hidden files starting with "."
  if (username[0] == '.')
    return false;

  for (size_t i = 1; i < username.length(); ++i) {
    if (username[i] == '.' && username[i - 1] == '.') {
      return false; // Found ".."
    }
  }

  // 3. Prevent Shell Injection & Enforce Linux Rules
  // Allowed: a-z, A-Z, 0-9, _, ., -, $ (samba)
  for (char c : username) {
    bool is_lower = (c >= 'a' && c <= 'z');
    bool is_upper = (c >= 'A' && c <= 'Z');
    bool is_digit = (c >= '0' && c <= '9');
    bool is_special = (c == '_' || c == '.' || c == '-' || c == '$');

    if (!(is_lower || is_upper || is_digit || is_special)) {
      return false;
    }
  }
  return true;
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
