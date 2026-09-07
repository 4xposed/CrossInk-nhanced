#include <MangaImageGeometry.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

using manga::ImageGeometry;
using manga::ImageViewport;

TEST(MangaImageGeometry, FitsAndCentersPortraitLandscapeSquareAndOddImages) {
  struct Case {
    int sourceWidth;
    int sourceHeight;
    ImageViewport viewport;
    ImageGeometry expected;
  };
  constexpr Case cases[] = {
      {1000, 1421, {0, 0, 480, 800}, {0, 59, 480, 682}},
      {1421, 1000, {10, 20, 800, 480}, {69, 20, 682, 480}},
      {100, 100, {5, 7, 301, 201}, {55, 7, 201, 201}},
      {7, 3, {11, 13, 5, 5}, {11, 14, 5, 2}},
      {3, 7, {11, 13, 5, 5}, {12, 13, 2, 5}},
  };

  for (const Case& testCase : cases) {
    ImageGeometry actual{};
    ASSERT_TRUE(manga::fitImage(testCase.sourceWidth, testCase.sourceHeight, testCase.viewport,
                                /*allowUpscale=*/true, actual));
    EXPECT_EQ(actual.x, testCase.expected.x);
    EXPECT_EQ(actual.y, testCase.expected.y);
    EXPECT_EQ(actual.width, testCase.expected.width);
    EXPECT_EQ(actual.height, testCase.expected.height);
  }
}

TEST(MangaImageGeometry, HonorsUpscalePolicy) {
  ImageGeometry geometry{};
  ASSERT_TRUE(manga::fitImage(3, 2, {10, 20, 8, 7}, /*allowUpscale=*/false, geometry));
  EXPECT_EQ(geometry.x, 12);
  EXPECT_EQ(geometry.y, 22);
  EXPECT_EQ(geometry.width, 3);
  EXPECT_EQ(geometry.height, 2);

  ASSERT_TRUE(manga::fitImage(3, 2, {10, 20, 8, 7}, /*allowUpscale=*/true, geometry));
  EXPECT_EQ(geometry.x, 10);
  EXPECT_EQ(geometry.y, 21);
  EXPECT_EQ(geometry.width, 8);
  EXPECT_EQ(geometry.height, 5);
}

TEST(MangaImageGeometry, KeepsEverySuccessfulFitContainedAndAspectRounded) {
  constexpr int dimensions[] = {1, 2, 3, 7, 31, 127, 479, 480, 799, 800, 4095, 32767};
  for (const int sourceWidth : dimensions) {
    for (const int sourceHeight : dimensions) {
      for (const int viewportWidth : dimensions) {
        for (const int viewportHeight : dimensions) {
          ImageGeometry geometry{};
          ASSERT_TRUE(manga::fitImage(sourceWidth, sourceHeight, {0, 0, viewportWidth, viewportHeight},
                                      /*allowUpscale=*/true, geometry));
          EXPECT_GE(geometry.x, 0);
          EXPECT_GE(geometry.y, 0);
          EXPECT_GE(geometry.width, 1);
          EXPECT_GE(geometry.height, 1);
          EXPECT_LE(static_cast<int64_t>(geometry.x) + geometry.width, viewportWidth);
          EXPECT_LE(static_cast<int64_t>(geometry.y) + geometry.height, viewportHeight);

          const int64_t aspectError =
              geometry.width * static_cast<int64_t>(sourceHeight) - geometry.height * static_cast<int64_t>(sourceWidth);
          const int64_t absoluteError = aspectError < 0 ? -aspectError : aspectError;
          // One fitted dimension is rounded to the nearest pixel. A one-pixel
          // clamp is allowed for ratios whose other dimension rounds to zero.
          EXPECT_LE(absoluteError, static_cast<int64_t>(sourceWidth > sourceHeight ? sourceWidth : sourceHeight));
        }
      }
    }
  }
}

TEST(MangaImageGeometry, RejectsInvalidOrUnrepresentableInputsAndClearsOutput) {
  constexpr int tooLarge = std::numeric_limits<int16_t>::max() + 1;
  constexpr ImageViewport validViewport{0, 0, 480, 800};
  const struct {
    int sourceWidth;
    int sourceHeight;
    ImageViewport viewport;
  } invalid[] = {
      {0, 1, validViewport},        {1, 0, validViewport},        {-1, 1, validViewport},   {1, -1, validViewport},
      {tooLarge, 1, validViewport}, {1, tooLarge, validViewport}, {1, 1, {0, 0, 0, 1}},     {1, 1, {0, 0, 1, 0}},
      {1, 1, {0, 0, tooLarge, 1}},  {1, 1, {0, 0, 1, tooLarge}},  {1, 1, {-1, 0, 1, 1}},    {1, 1, {0, -1, 1, 1}},
      {1, 1, {32767, 0, 1, 1}},     {1, 1, {0, 32767, 1, 1}},     {1, 1, {32760, 0, 8, 1}},
  };

  for (const auto& testCase : invalid) {
    ImageGeometry geometry{91, 92, 93, 94};
    EXPECT_FALSE(manga::fitImage(testCase.sourceWidth, testCase.sourceHeight, testCase.viewport,
                                 /*allowUpscale=*/true, geometry));
    EXPECT_EQ(geometry.x, 0);
    EXPECT_EQ(geometry.y, 0);
    EXPECT_EQ(geometry.width, 0);
    EXPECT_EQ(geometry.height, 0);
  }
}

TEST(MangaImageGeometry, KeepsBoundarySizedGeometryInRange) {
  constexpr int max = std::numeric_limits<int16_t>::max();
  ImageGeometry geometry{};
  ASSERT_TRUE(manga::fitImage(max, max, {0, 0, max, max}, /*allowUpscale=*/true, geometry));
  EXPECT_EQ(geometry.x, 0);
  EXPECT_EQ(geometry.y, 0);
  EXPECT_EQ(geometry.width, max);
  EXPECT_EQ(geometry.height, max);
}

TEST(MangaImageGeometry, RotatesOnlyNonSquareAspectMismatchesWhenAllowed) {
  EXPECT_TRUE(manga::shouldRotateImage(1200, 600, 480, 800, true));
  EXPECT_TRUE(manga::shouldRotateImage(600, 1200, 800, 480, true));
  EXPECT_FALSE(manga::shouldRotateImage(1200, 600, 800, 480, true));
  EXPECT_FALSE(manga::shouldRotateImage(600, 1200, 480, 800, true));
  EXPECT_FALSE(manga::shouldRotateImage(1200, 600, 480, 800, false));
  EXPECT_FALSE(manga::shouldRotateImage(600, 600, 480, 800, true));
  EXPECT_FALSE(manga::shouldRotateImage(600, 1200, 800, 800, true));
}

TEST(MangaImageGeometry, AppliesPinnedCounterClockwiseOrientationRotation) {
  EXPECT_EQ(manga::rotatedImageOrientation(0), 3);
  EXPECT_EQ(manga::rotatedImageOrientation(1), 0);
  EXPECT_EQ(manga::rotatedImageOrientation(2), 1);
  EXPECT_EQ(manga::rotatedImageOrientation(3), 2);
}

TEST(MangaImageGeometry, RejectsInvalidRotationDimensions) {
  constexpr int tooLarge = std::numeric_limits<int16_t>::max() + 1;
  EXPECT_FALSE(manga::shouldRotateImage(0, 1, 480, 800, true));
  EXPECT_FALSE(manga::shouldRotateImage(1, -1, 480, 800, true));
  EXPECT_FALSE(manga::shouldRotateImage(1, 1, 0, 800, true));
  EXPECT_FALSE(manga::shouldRotateImage(1, 1, 480, tooLarge, true));
}

}  // namespace
