#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace fs = std::filesystem;

// Copy of getModelVersion function for testing
inline std::string getModelVersion(const std::string &model_path) {
  fs::path p(model_path);
  std::string filename = p.stem().string();
  const std::string prefix = "face_recognition_";
  if (filename.rfind(prefix, 0) == 0) {
    return filename.substr(prefix.length());
  }
  return filename;
}

TEST(ModelVersionTest, StandardSFaceModel) {
  EXPECT_EQ(getModelVersion(
                "/etc/linuxcampam/models/face_recognition_sface_2021dec.onnx"),
            "sface_2021dec");
}

TEST(ModelVersionTest, FutureModelVersion) {
  EXPECT_EQ(getModelVersion("/path/to/face_recognition_sface_2024.onnx"),
            "sface_2024");
}

TEST(ModelVersionTest, CustomModelWithoutPrefix) {
  EXPECT_EQ(getModelVersion("/models/custom_recognizer_v2.onnx"),
            "custom_recognizer_v2");
}

TEST(ModelVersionTest, RelativePath) {
  EXPECT_EQ(getModelVersion("models/face_recognition_arcface.onnx"), "arcface");
}

TEST(ModelVersionTest, JustFilename) {
  EXPECT_EQ(getModelVersion("face_recognition_vggface.onnx"), "vggface");
}

TEST(ModelVersionTest, NoExtension) {
  EXPECT_EQ(getModelVersion("/path/face_recognition_test"), "test");
}
