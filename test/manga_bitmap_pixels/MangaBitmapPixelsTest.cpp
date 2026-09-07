#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "HalStorage.h"
#include "MangaBitmapPixels.h"

namespace {
void put16(std::vector<uint8_t>& bytes, size_t at, uint16_t value) {
  bytes[at] = static_cast<uint8_t>(value);
  bytes[at + 1] = static_cast<uint8_t>(value >> 8);
}
void put32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) bytes[at + i] = static_cast<uint8_t>(value >> (i * 8));
}

// Four logical rows of five 2-bit pixels. The palette maps indices directly to
// black/dark/light/white, so Bitmap's native-palette path has no error diffusion.
void write2BitBmp(const std::string& path, bool topDown) {
  constexpr int width = 5, height = 4, rowBytes = 4, pixelOffset = 70;
  const uint8_t rows[height][width] = {{0, 1, 2, 3, 0}, {3, 2, 1, 0, 3}, {1, 3, 0, 2, 1}, {2, 0, 3, 1, 2}};
  std::vector<uint8_t> bytes(pixelOffset + rowBytes * height, 0);
  put16(bytes, 0, 0x4d42);
  put32(bytes, 2, static_cast<uint32_t>(bytes.size()));
  put32(bytes, 10, pixelOffset);
  put32(bytes, 14, 40);
  put32(bytes, 18, width);
  put32(bytes, 22, topDown ? static_cast<uint32_t>(-height) : height);
  put16(bytes, 26, 1);
  put16(bytes, 28, 2);
  put32(bytes, 34, rowBytes * height);
  put32(bytes, 46, 4);
  for (int i = 0; i < 4; ++i) {
    const uint8_t lum = static_cast<uint8_t>(i * 85);
    bytes[54 + i * 4] = lum;
    bytes[55 + i * 4] = lum;
    bytes[56 + i * 4] = lum;
  }
  for (int physical = 0; physical < height; ++physical) {
    const int logical = topDown ? physical : height - 1 - physical;
    for (int x = 0; x < width; ++x) {
      bytes[pixelOffset + physical * rowBytes + (x >> 2)] |= rows[logical][x] << (6 - 2 * (x & 3));
    }
  }
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void write24BitBmp(const std::string& path, bool topDown) {
  constexpr int width = 3, height = 3, rowBytes = 12, pixelOffset = 54;
  const uint8_t gray[height][width] = {{10, 60, 150}, {220, 100, 30}, {80, 180, 250}};
  std::vector<uint8_t> bytes(pixelOffset + rowBytes * height, 0);
  put16(bytes, 0, 0x4d42);
  put32(bytes, 2, static_cast<uint32_t>(bytes.size()));
  put32(bytes, 10, pixelOffset);
  put32(bytes, 14, 40);
  put32(bytes, 18, width);
  put32(bytes, 22, topDown ? static_cast<uint32_t>(-height) : height);
  put16(bytes, 26, 1);
  put16(bytes, 28, 24);
  put32(bytes, 34, rowBytes * height);
  for (int physical = 0; physical < height; ++physical) {
    const int logical = topDown ? physical : height - 1 - physical;
    for (int x = 0; x < width; ++x) {
      uint8_t* pixel = bytes.data() + pixelOffset + physical * rowBytes + x * 3;
      pixel[0] = pixel[1] = pixel[2] = gray[logical][x];
    }
  }
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::vector<uint8_t> readAll(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

class MangaBitmapPixelsTest : public testing::Test {
 protected:
  void SetUp() override {
    directory = std::filesystem::temp_directory_path() / "crossink_manga_bitmap_pixels";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    storage_test::failWrite = storage_test::failSync = storage_test::failClose = false;
  }
  void TearDown() override { std::filesystem::remove_all(directory); }
  std::filesystem::path directory;
  std::array<uint8_t, manga::kBitmapPixelScratchBytes> scratch{};
};

TEST_F(MangaBitmapPixelsTest, WritesLogicalTopDownFourLevelRowsForBothBmpOrders) {
  const auto top = (directory / "top.bmp").string();
  const auto bottom = (directory / "bottom.bmp").string();
  const auto topOut = (directory / "top.pxc.tmp").string();
  const auto bottomOut = (directory / "bottom.pxc.tmp").string();
  write2BitBmp(top, true);
  write2BitBmp(bottom, false);

  ASSERT_TRUE(manga::writeBitmapPixels(top.c_str(), topOut.c_str(), 5, 4, scratch.data(), scratch.size()));
  ASSERT_TRUE(manga::writeBitmapPixels(bottom.c_str(), bottomOut.c_str(), 5, 4, scratch.data(), scratch.size()));
  const std::vector<uint8_t> expected = {5, 0, 4, 0, 0x1b, 0x00, 0xe4, 0xc0, 0x72, 0x40, 0x8d, 0x80};
  EXPECT_EQ(readAll(topOut), expected);
  EXPECT_EQ(readAll(bottomOut), expected);
}

TEST_F(MangaBitmapPixelsTest, DownscalesOddDimensionsAtFloorMappedColorPositions) {
  const auto source = (directory / "source.bmp").string();
  const auto output = (directory / "out.pxc.tmp").string();
  write2BitBmp(source, true);
  ASSERT_TRUE(manga::writeBitmapPixels(source.c_str(), output.c_str(), 3, 2, scratch.data(), scratch.size()));
  const std::vector<uint8_t> expected = {3, 0, 2, 0, 0x1c, 0x78};
  EXPECT_EQ(readAll(output), expected);
}

TEST_F(MangaBitmapPixelsTest, HighColorQuantizationIsIndependentOfBmpRowOrder) {
  const auto top = (directory / "top24.bmp").string();
  const auto bottom = (directory / "bottom24.bmp").string();
  const auto topOut = (directory / "top24.pxc.tmp").string();
  const auto bottomOut = (directory / "bottom24.pxc.tmp").string();
  write24BitBmp(top, true);
  write24BitBmp(bottom, false);
  ASSERT_TRUE(manga::writeBitmapPixels(top.c_str(), topOut.c_str(), 3, 3, scratch.data(), scratch.size()));
  ASSERT_TRUE(manga::writeBitmapPixels(bottom.c_str(), bottomOut.c_str(), 3, 3, scratch.data(), scratch.size()));
  EXPECT_EQ(readAll(topOut), readAll(bottomOut));
}

TEST_F(MangaBitmapPixelsTest, ProbeRejectsExtendedDibOverlappingPixelsAndTruncatedPayload) {
  const auto source = (directory / "source.bmp").string();
  write2BitBmp(source, true);
  manga::BitmapPixelInfo info{};

  auto bytes = readAll(source);
  put32(bytes, 14, 108);
  std::ofstream(source, std::ios::binary | std::ios::trunc)
      .write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  EXPECT_FALSE(manga::probeBitmapPixels(source.c_str(), info));

  write2BitBmp(source, true);
  bytes = readAll(source);
  put32(bytes, 10, 54);
  std::ofstream(source, std::ios::binary | std::ios::trunc)
      .write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  EXPECT_FALSE(manga::probeBitmapPixels(source.c_str(), info));

  write2BitBmp(source, true);
  std::filesystem::resize_file(source, std::filesystem::file_size(source) - 1);
  EXPECT_FALSE(manga::probeBitmapPixels(source.c_str(), info));
}

TEST_F(MangaBitmapPixelsTest, RejectsSmallScratchAndUpscaleWithoutLeavingOutput) {
  const auto source = (directory / "source.bmp").string();
  const auto output = (directory / "out.pxc.tmp").string();
  write2BitBmp(source, true);
  EXPECT_FALSE(manga::writeBitmapPixels(source.c_str(), output.c_str(), 5, 4, scratch.data(), 7));
  EXPECT_FALSE(std::filesystem::exists(output));
  EXPECT_FALSE(manga::writeBitmapPixels(source.c_str(), output.c_str(), 6, 4, scratch.data(), scratch.size()));
  EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(MangaBitmapPixelsTest, RemovesPartialOutputOnShortSourceAndOutputFailures) {
  const auto source = (directory / "source.bmp").string();
  const auto output = (directory / "out.pxc.tmp").string();
  write2BitBmp(source, true);
  std::filesystem::resize_file(source, std::filesystem::file_size(source) - 2);
  EXPECT_FALSE(manga::writeBitmapPixels(source.c_str(), output.c_str(), 5, 4, scratch.data(), scratch.size()));
  EXPECT_FALSE(std::filesystem::exists(output));

  write2BitBmp(source, true);
  storage_test::failWrite = true;
  EXPECT_FALSE(manga::writeBitmapPixels(source.c_str(), output.c_str(), 5, 4, scratch.data(), scratch.size()));
  EXPECT_FALSE(std::filesystem::exists(output));
  storage_test::failWrite = false;
  storage_test::failSync = true;
  EXPECT_FALSE(manga::writeBitmapPixels(source.c_str(), output.c_str(), 5, 4, scratch.data(), scratch.size()));
  EXPECT_FALSE(std::filesystem::exists(output));
  storage_test::failSync = false;
  storage_test::failClose = true;
  EXPECT_FALSE(manga::writeBitmapPixels(source.c_str(), output.c_str(), 5, 4, scratch.data(), scratch.size()));
  EXPECT_FALSE(std::filesystem::exists(output));
}
TEST_F(MangaBitmapPixelsTest, CancellationBeforeAndBetweenRowsRemovesTemporary) {
  const auto source = (directory / "cancel.bmp").string();
  const auto output = (directory / "cancel.pxc.tmp").string();
  write2BitBmp(source, true);
  for (int checks : {0, 3, 5, 6}) {
    int left = checks;
    CooperativeCancellation cancel{[](void* p) {
                                     int& count = *static_cast<int*>(p);
                                     return count-- <= 0;
                                   },
                                   &left};
    EXPECT_FALSE(
        manga::writeBitmapPixels(source.c_str(), output.c_str(), 5, 4, scratch.data(), scratch.size(), cancel));
    EXPECT_FALSE(std::filesystem::exists(output));
  }
}

}  // namespace
