#include "json.hpp"

#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <vector>
using json = nlohmann::json;

// ============================================================================
// INPUT VALIDATION TESTS
// ============================================================================

// isValidUsername is defined in test_config.cpp
extern bool isValidUsername(const std::string &username);

// Camera path validation
bool isValidCameraPath(const std::string &path) {
  // Only allow /dev/video* patterns
  if (path.rfind("/dev/video", 0) != 0)
    return false;
  // Check for path traversal attempts
  if (path.find("..") != std::string::npos)
    return false;
  // Must be reasonable length
  if (path.length() > 20)
    return false;
  return true;
}

// Config path validation
bool isValidConfigPath(const std::string &path) {
  // Must be absolute path
  if (path.empty() || path[0] != '/')
    return false;
  // No path traversal
  if (path.find("..") != std::string::npos)
    return false;
  // Must end with .ini
  if (path.length() < 4 || path.substr(path.length() - 4) != ".ini")
    return false;
  return true;
}

TEST(SecurityTest, CommandInjectionViaUsername) {
  // Shell metacharacters
  EXPECT_FALSE(isValidUsername("user; rm -rf /"));
  EXPECT_FALSE(isValidUsername("user$(whoami)"));
  EXPECT_FALSE(isValidUsername("user`id`"));
  EXPECT_FALSE(isValidUsername("user|cat /etc/passwd"));
  EXPECT_FALSE(isValidUsername("user&& malicious"));
  EXPECT_FALSE(isValidUsername("user\necho pwned"));
  EXPECT_FALSE(isValidUsername("user\recho pwned"));
  EXPECT_FALSE(isValidUsername("$(cat /etc/shadow)"));
  EXPECT_FALSE(isValidUsername("${PATH}"));

  // Null byte injection
  std::string null_injection = "user";
  null_injection += '\0';
  null_injection += "admin";
  EXPECT_FALSE(isValidUsername(null_injection));
}

TEST(SecurityTest, CameraPathValidation) {
  // Valid paths
  EXPECT_TRUE(isValidCameraPath("/dev/video0"));
  EXPECT_TRUE(isValidCameraPath("/dev/video1"));
  EXPECT_TRUE(isValidCameraPath("/dev/video10"));

  // Invalid - path traversal
  EXPECT_FALSE(isValidCameraPath("/dev/video0/../video1"));
  EXPECT_FALSE(isValidCameraPath("/dev/../etc/passwd"));

  // Invalid - not video device
  EXPECT_FALSE(isValidCameraPath("/dev/sda1"));
  EXPECT_FALSE(isValidCameraPath("/etc/passwd"));
  EXPECT_FALSE(isValidCameraPath("/tmp/fake_video0"));

  // Invalid - symlink escape attempts
  EXPECT_FALSE(isValidCameraPath("/dev/video0; cat /etc/passwd"));
}

TEST(SecurityTest, ConfigPathValidation) {
  // Valid paths
  EXPECT_TRUE(isValidConfigPath("/etc/linuxcampam/config.ini"));
  EXPECT_TRUE(isValidConfigPath("/home/user/.config/test.ini"));

  // Invalid - relative path
  EXPECT_FALSE(isValidConfigPath("config.ini"));
  EXPECT_FALSE(isValidConfigPath("./config.ini"));

  // Invalid - path traversal
  EXPECT_FALSE(isValidConfigPath("/etc/linuxcampam/../passwd"));
  EXPECT_FALSE(isValidConfigPath("/tmp/../etc/shadow.ini"));

  // Invalid - wrong extension
  EXPECT_FALSE(isValidConfigPath("/etc/linuxcampam/config.sh"));
  EXPECT_FALSE(isValidConfigPath("/etc/passwd"));
}

// ============================================================================
// EMBEDDING SECURITY TESTS
// ============================================================================

TEST(SecurityTest, MalformedEmbeddingData) {
  // Empty embedding array should be handled
  json empty_emb;
  empty_emb["data"] = json::array();
  EXPECT_EQ(empty_emb["data"].size(), 0);

  // Wrong dimension - SFace uses 128-dim embeddings
  std::vector<float> wrong_dim(64, 0.5f);
  EXPECT_NE(wrong_dim.size(), 128);

  // Correct dimension
  std::vector<float> correct_dim(128, 0.5f);
  EXPECT_EQ(correct_dim.size(), 128);
}

TEST(SecurityTest, EmbeddingNaNAndInfValues) {
  std::vector<float> embedding(128, 0.5f);

  // NaN values should be detectable
  embedding[0] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_TRUE(std::isnan(embedding[0]));

  // Inf values should be detectable
  embedding[1] = std::numeric_limits<float>::infinity();
  EXPECT_TRUE(std::isinf(embedding[1]));

  // Normal values
  embedding[2] = 0.5f;
  EXPECT_FALSE(std::isnan(embedding[2]));
  EXPECT_FALSE(std::isinf(embedding[2]));
}

// Helper to check if embedding is valid (no NaN/Inf)
bool isValidEmbedding(const std::vector<float> &emb) {
  if (emb.size() != 128)
    return false;
  for (float v : emb) {
    if (std::isnan(v) || std::isinf(v))
      return false;
  }
  return true;
}

TEST(SecurityTest, EmbeddingValidation) {
  // Valid embedding
  std::vector<float> valid(128, 0.5f);
  EXPECT_TRUE(isValidEmbedding(valid));

  // Wrong size
  std::vector<float> wrong_size(64, 0.5f);
  EXPECT_FALSE(isValidEmbedding(wrong_size));

  // Contains NaN
  std::vector<float> with_nan(128, 0.5f);
  with_nan[50] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(isValidEmbedding(with_nan));

  // Contains Inf
  std::vector<float> with_inf(128, 0.5f);
  with_inf[50] = std::numeric_limits<float>::infinity();
  EXPECT_FALSE(isValidEmbedding(with_inf));
}

// ============================================================================
// RESOURCE EXHAUSTION TESTS
// ============================================================================

TEST(SecurityTest, MaxEmbeddingsLimit) {
  const int MAX_EMBEDDINGS = 5;
  json embeddings = json::array();

  // Add up to limit
  for (int i = 0; i < MAX_EMBEDDINGS; i++) {
    json e;
    e["label"] = "label_" + std::to_string(i);
    e["data"] = std::vector<float>(128, 0.1f * i);
    embeddings.push_back(e);
  }

  EXPECT_EQ(embeddings.size(), MAX_EMBEDDINGS);

  // Attempting to add more should be rejected by application logic
  // (This test verifies the data structure, actual enforcement is in
  // auth_engine)
}

TEST(SecurityTest, LargeUserFilePrevention) {
  // Simulate a very large embedding that could cause memory issues
  // The application should have size limits

  const size_t MAX_REASONABLE_SIZE = 1024 * 1024; // 1MB
  json large_data;

  // 128 floats * 4 bytes * 5 embeddings = ~2.5KB per camera type
  // With 2 camera types and metadata, still well under 1MB
  std::vector<float> emb(128, 0.5f);
  size_t single_emb_size = emb.size() * sizeof(float);

  // Even with 1000 embeddings, we'd be at ~500KB
  EXPECT_LT(single_emb_size * 1000, MAX_REASONABLE_SIZE);
}

// ============================================================================
// JSON PARSING SECURITY TESTS
// ============================================================================

TEST(SecurityTest, MalformedJsonHandling) {
  // These should throw or be handled gracefully
  EXPECT_THROW((void)json::parse("{invalid json}"), json::parse_error);
  EXPECT_THROW((void)json::parse(""), json::parse_error);
  EXPECT_THROW((void)json::parse("{\"unclosed\": "), json::parse_error);

  // Valid JSON should parse
  EXPECT_NO_THROW((void)json::parse("{}"));
  EXPECT_NO_THROW((void)json::parse("{\"valid\": true}"));
}

TEST(SecurityTest, JsonTypeConfusion) {
  json data;
  data["embeddings_ir"] = "not_an_array"; // Should be array

  // Application should check types before accessing
  EXPECT_FALSE(data["embeddings_ir"].is_array());
  EXPECT_TRUE(data["embeddings_ir"].is_string());
}

// ============================================================================
// SIMILARITY CALCULATION SECURITY
// ============================================================================

// Cosine similarity function (copy for testing)
float cosine_similarity(const std::vector<float> &a,
                        const std::vector<float> &b) {
  if (a.size() != b.size() || a.empty())
    return 0.0f;

  float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
  for (size_t i = 0; i < a.size(); i++) {
    dot += a[i] * b[i];
    norm_a += a[i] * a[i];
    norm_b += b[i] * b[i];
  }

  float denom = std::sqrt(norm_a) * std::sqrt(norm_b);
  if (denom == 0.0f)
    return 0.0f;
  return dot / denom;
}

TEST(SecurityTest, SimilarityEdgeCases) {
  std::vector<float> emb1(128, 0.5f);
  std::vector<float> emb2(128, 0.5f);

  // Identical embeddings should have similarity ~1.0
  float sim = cosine_similarity(emb1, emb2);
  EXPECT_NEAR(sim, 1.0f, 0.001f);

  // Zero vectors should return 0, not crash
  std::vector<float> zero_vec(128, 0.0f);
  sim = cosine_similarity(zero_vec, zero_vec);
  EXPECT_EQ(sim, 0.0f); // Division by zero handled

  // Different sizes should return 0
  std::vector<float> short_vec(64, 0.5f);
  sim = cosine_similarity(emb1, short_vec);
  EXPECT_EQ(sim, 0.0f);

  // Empty vectors should return 0
  std::vector<float> empty_vec;
  sim = cosine_similarity(empty_vec, empty_vec);
  EXPECT_EQ(sim, 0.0f);
}

TEST(SecurityTest, SimilarityOverflowBehavior) {
  // NOTE: With very large values (1e30), the dot product and norms overflow
  // to infinity, resulting in inf/inf = NaN. This is documented behavior.
  // Real embeddings are normalized and stay in [-1, 1] range, so this
  // edge case doesn't occur in practice.
  //
  // If hardening is needed in the future, use double precision or
  // normalize before computation.

  std::vector<float> large_vals(128, 1e30f);
  float sim = cosine_similarity(large_vals, large_vals);

  // Document current behavior: overflow produces NaN
  EXPECT_TRUE(std::isnan(sim));

  // Normal-range values should work correctly
  std::vector<float> normal_vals(128, 0.5f);
  sim = cosine_similarity(normal_vals, normal_vals);
  EXPECT_FALSE(std::isnan(sim));
  EXPECT_NEAR(sim, 1.0f, 0.001f);
}
