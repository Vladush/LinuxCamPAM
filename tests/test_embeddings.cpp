#include <algorithm>
#include <ctime>
#include <gtest/gtest.h>
#include <string>
#include <vector>

// Using nlohmann/json inline for testing
#include "json.hpp"
using json = nlohmann::json;

namespace {
constexpr size_t kMaxLabelLength = 32;
constexpr size_t kMockEmbeddingSize = 128;
constexpr float kVal01 = 0.1F;
constexpr float kVal02 = 0.2F;
constexpr float kVal03 = 0.3F;
constexpr float kVal04 = 0.4F;
constexpr float kVal05 = 0.5F;
} // namespace

// Label validation (copy for testing)
bool isValidLabel(const std::string &label) {
  if (label.empty() || label.length() > kMaxLabelLength)
    return false;
  return std::all_of(label.begin(), label.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
  });
}

TEST(LabelValidationTest, ValidLabels) {
  EXPECT_TRUE(isValidLabel("default"));
  EXPECT_TRUE(isValidLabel("daylight"));
  EXPECT_TRUE(isValidLabel("glasses-on"));
  EXPECT_TRUE(isValidLabel("low_light_2"));
}

TEST(LabelValidationTest, InvalidLabels) {
  EXPECT_FALSE(isValidLabel(""));
  EXPECT_FALSE(isValidLabel("label with spaces"));
  EXPECT_FALSE(isValidLabel("label/slash"));
  EXPECT_FALSE(isValidLabel("label..dots"));
  EXPECT_FALSE(
      isValidLabel("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")); // >32 chars
}

TEST(EmbeddingFormatTest, SingleEmbeddingStructure) {
  json entry = {
      {"label", "default"},
      {"data", std::vector<float>{kVal01, kVal02, kVal03}},
      {"created", std::time(nullptr)},
      {"model_version", "sface_2021dec"}
  };

  EXPECT_TRUE(entry.contains("label"));
  EXPECT_TRUE(entry.contains("data"));
  EXPECT_TRUE(entry.contains("created"));
  EXPECT_TRUE(entry.contains("model_version"));
  EXPECT_EQ(entry.at("label"), "default");
  EXPECT_EQ(entry.at("model_version"), "sface_2021dec");
}

TEST(EmbeddingFormatTest, MultiEmbeddingArray) {
  json embeddings = json::array();

  json e1 = {
      {"label", "default"},
      {"data", std::vector<float>{kVal01, kVal02}},
      {"model_version", "sface_2021dec"}
  };
  embeddings.push_back(e1);

  json e2 = {
      {"label", "glasses"},
      {"data", std::vector<float>{kVal03, kVal04}},
      {"model_version", "sface_2021dec"}
  };
  embeddings.push_back(e2);

  EXPECT_EQ(embeddings.size(), 2);
  EXPECT_EQ(embeddings.at(0).at("label"), "default");
  EXPECT_EQ(embeddings.at(1).at("label"), "glasses");
}

TEST(EmbeddingFormatTest, UserFileStructure) {
  json user_data = {
      {"embeddings_ir", json::array()},
      {"embeddings_rgb", json::array()}
  };

  json ir_emb = {
      {"label", "default"},
      {"data", std::vector<float>(kMockEmbeddingSize, kVal05)},
      {"model_version", "sface_2021dec"}
  };
  user_data.at("embeddings_ir").push_back(ir_emb);

  EXPECT_TRUE(user_data.contains("embeddings_ir"));
  EXPECT_TRUE(user_data.contains("embeddings_rgb"));
  EXPECT_EQ(user_data.at("embeddings_ir").size(), 1);
  EXPECT_EQ(user_data.at("embeddings_rgb").size(), 0);
}

TEST(EmbeddingFormatTest, ParseEmbeddingData) {
  std::string json_str = R"({
    "label": "test",
    "data": [0.1, 0.2, 0.3, 0.4, 0.5],
    "model_version": "sface_2021dec"
  })";

  json entry = json::parse(json_str);
  auto data = entry.at("data").get<std::vector<float>>();

  EXPECT_EQ(data.size(), 5);
  EXPECT_FLOAT_EQ(data.at(0), kVal01);
  EXPECT_FLOAT_EQ(data.at(4), kVal05);
}
