#include <gtest/gtest.h>
#include <regex>
#include <string>

bool isValidUsername(const std::string &username) {
  if (username.empty() || username.length() > 32)
    return false;
  // Specific check for Path Traversal ".."
  if (username == ".." || username == ".")
    return false;
  // Allow a-z, A-Z, 0-9, ., -, _
  static const std::regex user_regex("^[a-zA-Z0-9_\\.-]+$");
  return std::regex_match(username, user_regex);
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
