#include <GfxRenderer.h>
#include <HalStorage.h>
#include <JPEGDEC.h>  // HAS_NEON
#include <JpegToFramebufferConverter.h>
#include <PngToFramebufferConverter.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace fs = std::filesystem;
namespace {
bool cancelOnSync = false;
struct CancelAt {
  size_t bytes;
  bool fired = false;
  static bool check(void* p) {
    auto& self = *static_cast<CancelAt*>(p);
    self.fired |= storage_test::writtenBytes >= self.bytes;
    return self.fired;
  }
};
std::vector<uint8_t> readAll(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}
class ImageCacheDecodeTest : public ::testing::TestWithParam<bool> {
 protected:
  fs::path root;
  std::unique_ptr<ImageToFramebufferDecoder> decoder;
  std::string source;
  CacheDecodeConfig config{};
  void SetUp() override {
    char path[] = "/tmp/crossink-real-codec-XXXXXX";
    root = mkdtemp(path);
    storage_test::root = root;
    storage_test::failWrite = storage_test::failSync = storage_test::failClose = false;
    storage_test::failOpenWrite = false;
    storage_test::writtenBytes = storage_test::readBytes = 0;
    storage_test::failAfterBytes = SIZE_MAX;
    storage_test::onSync = nullptr;
    cancelOnSync = false;
    GfxRenderer::accesses = 0;
    if (GetParam())
      decoder = std::make_unique<JpegToFramebufferConverter>();
    else
      decoder = std::make_unique<PngToFramebufferConverter>();
    source = std::string(FIXTURE_DIR) + (GetParam() ? "/pattern.jpg" : "/pattern.png");
    config.render = {7, 9, 129, 193};
    config.render.useExactDimensions = true;
    config.render.cachePath = (root / "cache.tmp").string();
    config.screenWidth = config.screenHeight = 512;
  }
  void TearDown() override {
    EXPECT_EQ(storage_test::openFiles, 0);
    fs::remove_all(root);
  }
  void expectFailedClean() {
    EXPECT_FALSE(decoder->decodeToCache(source, config));
    EXPECT_FALSE(fs::exists(config.render.cachePath));
    EXPECT_EQ(storage_test::openFiles, 0);
    EXPECT_EQ(GfxRenderer::accesses, 0);
  }
};

TEST_P(ImageCacheDecodeTest, RealCodecCacheMatchesForegroundAcrossScalingDitherAndOrientation) {
  for (const auto& size : {std::pair{129, 193}, {65, 97}, {31, 47}, {201, 287}}) {
    for (bool dither : {false, true}) {
      for (int orientation = 0; orientation < 4; ++orientation) {
        config.render.maxWidth = size.first;
        config.render.maxHeight = size.second;
        config.render.useDithering = dither;
        GfxRenderer renderer;
        renderer.pixels.fill(255);
        renderer.orientation = static_cast<GfxRenderer::Orientation>(orientation);
        ASSERT_TRUE(decoder->decodeToFramebuffer(source, renderer, config.render));
        EXPECT_GT(GfxRenderer::accesses, 0);
        const auto foreground = readAll(config.render.cachePath);
        ASSERT_EQ(foreground.size(), 4u + ((size.first + 3) / 4) * size.second);
        fs::remove(config.render.cachePath);
        GfxRenderer::accesses = 0;
        ASSERT_TRUE(decoder->decodeToCache(source, config));
        EXPECT_EQ(readAll(config.render.cachePath), foreground);
        EXPECT_EQ(GfxRenderer::accesses, 0);
      }
    }
  }
}

// JPEGDEC's NEON IDCT (arm64 hosts) rounds differently from the scalar IDCT
// that x86 hosts and the ESP32-C3 run, so JPEG goldens exist for both.
#ifdef HAS_NEON
constexpr const char* kJpegGoldenFormat = "JPEG-neon";
#else
constexpr const char* kJpegGoldenFormat = "JPEG-scalar";
#endif

TEST_P(ImageCacheDecodeTest, MatchesOriginalForegroundGoldenHashes) {
  std::ifstream golden(std::string(FIXTURE_DIR) + "/foreground-fnv64.txt");
  ASSERT_TRUE(golden.good());
  std::string format;
  int width, height, dither;
  uint64_t expected;
  int checked = 0;
  while (golden >> format >> width >> height >> dither >> expected) {
    if (format != (GetParam() ? kJpegGoldenFormat : "PNG")) continue;
    ++checked;
    config.render.maxWidth = width;
    config.render.maxHeight = height;
    config.render.useDithering = dither;
    ASSERT_TRUE(decoder->decodeToCache(source, config));
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t byte : readAll(config.render.cachePath)) {
      hash ^= byte;
      hash *= 1099511628211ull;
    }
    EXPECT_EQ(hash, expected) << format << " " << width << "x" << height << " dither=" << dither;
  }
  EXPECT_EQ(checked, 8);
}

TEST_P(ImageCacheDecodeTest, EveryCancellationPollIncludingLastBlockAndFinalRowsAborts) {
  struct Polls {
    int count = 0;
    int stop = -1;
  } polls;
  config.cancellation = {[](void* p) {
                           auto& state = *static_cast<Polls*>(p);
                           return state.count++ == state.stop;
                         },
                         &polls};
  ASSERT_TRUE(decoder->decodeToCache(source, config));
  const int total = polls.count;
  fs::remove(config.render.cachePath);
  EXPECT_GT(total, 193);
  for (int stop = 0; stop < total; ++stop) {
    polls = {0, stop};
    SCOPED_TRACE(stop);
    expectFailedClean();
  }
}

TEST_P(ImageCacheDecodeTest, FinalRowWriteFailureCannotPublishCompleteLengthCache) {
  storage_test::failAfterBytes = 4 + ((129 + 3) / 4) * 192;
  expectFailedClean();
}

TEST_P(ImageCacheDecodeTest, CancellationBeforeEarlyMiddleAndBeyondTwoThirdsRemovesOutput) {
  const size_t payload = ((129 + 3) / 4) * 193;
  for (size_t threshold : {size_t(0), size_t(4), payload / 2, payload * 9 / 10}) {
    storage_test::writtenBytes = storage_test::readBytes = 0;
    CancelAt cancel{threshold};
    config.cancellation = {CancelAt::check, &cancel};
    expectFailedClean();
    EXPECT_TRUE(cancel.fired);
    if (threshold == 0)
      EXPECT_EQ(storage_test::readBytes, 0);
    else
      EXPECT_GE(storage_test::writtenBytes, threshold);
  }
}

TEST_P(ImageCacheDecodeTest, CancellationAfterDecodeDuringFinalSyncRemovesOutput) {
  storage_test::onSync = [] { cancelOnSync = true; };
  config.cancellation = {[](void*) { return cancelOnSync; }, nullptr};
  expectFailedClean();
  EXPECT_TRUE(cancelOnSync);
}

TEST_P(ImageCacheDecodeTest, CacheStartupAndMidstreamWriteFailuresAreNotSuccess) {
  storage_test::failOpenWrite = true;
  expectFailedClean();
  storage_test::failOpenWrite = false;
  storage_test::failWrite = true;
  expectFailedClean();
  storage_test::failWrite = false;
  storage_test::writtenBytes = 0;
  storage_test::failAfterBytes = 1200;
  expectFailedClean();
}

TEST_P(ImageCacheDecodeTest, SyncAndCloseFailuresRemoveOutput) {
  storage_test::failSync = true;
  expectFailedClean();
  storage_test::failSync = false;
  storage_test::failClose = true;
  expectFailedClean();
}

TEST_P(ImageCacheDecodeTest, ForegroundKeepsBwOutputWhenCacheFails) {
  GfxRenderer renderer;
  renderer.pixels.fill(255);
  storage_test::failOpenWrite = true;
  ASSERT_TRUE(decoder->decodeToFramebuffer(source, renderer, config.render));
  EXPECT_GT(GfxRenderer::accesses, 0);
  bool changed = false;
  for (auto byte : renderer.pixels) changed |= byte != 255;
  EXPECT_TRUE(changed);
  EXPECT_FALSE(fs::exists(config.render.cachePath));
}

TEST_P(ImageCacheDecodeTest, InvalidGeometryAndMalformedInputLeaveNoHandles) {
  config.render.useExactDimensions = false;
  expectFailedClean();
  config.render.useExactDimensions = true;
  source = (root / "broken").string();
  std::ofstream(source, std::ios::binary).write("broken image bytes", 18);
  expectFailedClean();
}
INSTANTIATE_TEST_SUITE_P(RealCodecs, ImageCacheDecodeTest, ::testing::Bool(),
                         [](const auto& info) { return info.param ? "JPEG" : "PNG"; });
}  // namespace
