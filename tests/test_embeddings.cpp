#include <ctime>
#include <gtest/gtest.h>
#include <string>
#include <vector>

// Using nlohmann/json inline for testing
#include "json.hpp"
using json = nlohmann::json;

// Label validation (copy for testing)
bool isValidLabel(const std::string &label) {
  if (label.empty() || label.length() > 32)
    return false;
  for (char c : label) {
    if (!std::isalnum(c) && c != '_' && c != '-')
      return false;
  }
  return true;
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
  json entry;
  entry["label"] = "default";
  entry["data"] = std::vector<float>{0.1f, 0.2f, 0.3f};
  entry["created"] = std::time(nullptr);
  entry["model_version"] = "sface_2021dec";

  EXPECT_TRUE(entry.contains("label"));
  EXPECT_TRUE(entry.contains("data"));
  EXPECT_TRUE(entry.contains("created"));
  EXPECT_TRUE(entry.contains("model_version"));
  EXPECT_EQ(entry["label"], "default");
  EXPECT_EQ(entry["model_version"], "sface_2021dec");
}

TEST(EmbeddingFormatTest, MultiEmbeddingArray) {
  json embeddings = json::array();

  json e1;
  e1["label"] = "default";
  e1["data"] = std::vector<float>{0.1f, 0.2f};
  e1["model_version"] = "sface_2021dec";
  embeddings.push_back(e1);

  json e2;
  e2["label"] = "glasses";
  e2["data"] = std::vector<float>{0.3f, 0.4f};
  e2["model_version"] = "sface_2021dec";
  embeddings.push_back(e2);

  EXPECT_EQ(embeddings.size(), 2);
  EXPECT_EQ(embeddings[0]["label"], "default");
  EXPECT_EQ(embeddings[1]["label"], "glasses");
}

TEST(EmbeddingFormatTest, UserFileStructure) {
  json user_data;
  user_data["embeddings_ir"] = json::array();
  user_data["embeddings_rgb"] = json::array();

  json ir_emb;
  ir_emb["label"] = "default";
  ir_emb["data"] = std::vector<float>(128, 0.5f);
  ir_emb["model_version"] = "sface_2021dec";
  user_data["embeddings_ir"].push_back(ir_emb);

  EXPECT_TRUE(user_data.contains("embeddings_ir"));
  EXPECT_TRUE(user_data.contains("embeddings_rgb"));
  EXPECT_EQ(user_data["embeddings_ir"].size(), 1);
  EXPECT_EQ(user_data["embeddings_rgb"].size(), 0);
}

TEST(EmbeddingFormatTest, ParseEmbeddingData) {
  std::string json_str = R"({
    "label": "test",
    "data": [0.1, 0.2, 0.3, 0.4, 0.5],
    "model_version": "sface_2021dec"
  })";

  json entry = json::parse(json_str);
  auto data = entry["data"].get<std::vector<float>>();

  EXPECT_EQ(data.size(), 5);
  EXPECT_FLOAT_EQ(data[0], 0.1f);
  EXPECT_FLOAT_EQ(data[4], 0.5f);
}
