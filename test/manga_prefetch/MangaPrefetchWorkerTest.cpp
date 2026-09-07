#include <GfxRenderer.h>
#include <HalStorage.h>
#include <MangaBitmapPixels.h>
#include <MangaPrefetch.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace {
const manga::ImageViewports views{{0, 0, 480, 800}, {0, 0, 800, 480}, 480, 800, 0};
void writeBmp() {
  std::vector<uint8_t> bytes(54 + 16 * 4, 0);
  auto put16 = [&](int pos, int value) {
    bytes[pos] = value;
    bytes[pos + 1] = value >> 8;
  };
  auto put32 = [&](int pos, int value) {
    for (int i = 0; i < 4; ++i) bytes[pos + i] = value >> (8 * i);
  };
  put16(0, 0x4d42);
  put32(2, bytes.size());
  put32(10, 54);
  put32(14, 40);
  put32(18, 5);
  put32(22, 4);
  put16(26, 1);
  put16(28, 24);
  for (size_t i = 54; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(i * 17);
  std::ofstream out(storage_test::root + "/page.bmp", std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
class MangaPrefetchWorker : public testing::Test {
 protected:
  void SetUp() override {
    storage_test::root = "/private/tmp/crossink-prefetch-worker-native";
    std::filesystem::remove_all(storage_test::root);
    std::filesystem::create_directories(storage_test::root);
    task_test::failCreate = false;
    task_test::pause = false;
    writeBmp();
  }
  void TearDown() override {
    task_test::pause = false;
    if (task_test::thread.joinable()) task_test::thread.join();
    EXPECT_EQ(storage_test::openFiles, 0);
    storage_test::onSync = nullptr;
    std::filesystem::remove_all(storage_test::root);
  }
  bool await(manga::MangaPrefetch& worker, bool& failed) {
    for (int i = 0; i < 1000; ++i) {
      if (worker.poll(failed)) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  }
};
TEST_F(MangaPrefetchWorker, TaskFailureLeavesForegroundFilesUsable) {
  task_test::failCreate = true;
  manga::MangaPrefetch worker("/book", 256);
  EXPECT_FALSE(worker.start());
  EXPECT_FALSE(worker.post("/private/tmp/crossink-prefetch-worker-native/page.bmp", 1, -1, true, views));
  manga::BitmapPixelInfo info;
  EXPECT_TRUE(manga::probeBitmapPixels("/private/tmp/crossink-prefetch-worker-native/page.bmp", info));
}
TEST_F(MangaPrefetchWorker, CopiedPathAndBmpPixelsMatchForeground) {
  manga::MangaPrefetch worker("/book", 256);
  ASSERT_TRUE(worker.start());
  char path[] = "/private/tmp/crossink-prefetch-worker-native/page.bmp";
  ASSERT_TRUE(worker.post(path, 1, -1, true, views));
  path[1] = 'x';
  EXPECT_FALSE(worker.post("/absent.bmp", 2, -1, true, views));
  bool failed = true;
  ASSERT_TRUE(await(worker, failed));
  ASSERT_FALSE(failed);
  EXPECT_EQ(storage_test::openFiles, 0);
  manga::PixelIdentity identity;
  ASSERT_TRUE(manga::MangaPixelCache::sourceIdentity("/book", 1, -1, identity));
  EXPECT_EQ(identity.sourceWidth, 5);
  EXPECT_EQ(identity.width, 5);  // BMP does not upscale.
  EXPECT_EQ(identity.orientation, 3);
  manga::MangaPixelCache cache;
  ASSERT_TRUE(cache.configure("/book", 1, -1, identity));
  ASSERT_TRUE(cache.open());
  std::vector<uint8_t> warm(8);
  for (int row = 0; row < 4; ++row) ASSERT_TRUE(cache.readRow(warm.data() + row * 2, 2));
  cache.close();
  std::vector<uint8_t> scratch(manga::kBitmapPixelScratchBytes);
  ASSERT_TRUE(manga::writeBitmapPixels("/private/tmp/crossink-prefetch-worker-native/page.bmp",
                                       "/private/tmp/crossink-prefetch-worker-native/cold.pxc", 5, 4, scratch.data(),
                                       scratch.size()));
  std::ifstream file(storage_test::root + "/cold.pxc", std::ios::binary);
  std::vector<uint8_t> cold((std::istreambuf_iterator<char>(file)), {});
  ASSERT_EQ(cold.size(), warm.size() + 4);
  EXPECT_TRUE(std::equal(warm.begin(), warm.end(), cold.begin() + 4));
  ASSERT_TRUE(worker.post("/private/tmp/crossink-prefetch-worker-native/page.bmp", 1, -1, true,
                          views));  // Valid cache hit also closes reader.
  ASSERT_TRUE(await(worker, failed));
  EXPECT_FALSE(failed);
}
TEST_F(MangaPrefetchWorker, CancelPostedGenerationAndRetainSlotUntilAcknowledged) {
  task_test::pause = true;
  manga::MangaPrefetch worker("/book", 256);
  ASSERT_TRUE(worker.start());
  ASSERT_TRUE(worker.post("/private/tmp/crossink-prefetch-worker-native/page.bmp", 1, -1, true, views));
  worker.cancel();
  EXPECT_FALSE(worker.idle());
  EXPECT_FALSE(worker.post("/private/tmp/crossink-prefetch-worker-native/page.bmp", 2, -1, true, views));
  task_test::pause = false;
  bool failed = false;
  ASSERT_TRUE(await(worker, failed));
  manga::PixelIdentity identity;
  EXPECT_FALSE(manga::MangaPixelCache::sourceIdentity("/book", 1, -1, identity));
  EXPECT_TRUE(worker.idle());
}
TEST_F(MangaPrefetchWorker, ShutdownJoinsBeforeSourceOwnerDestruction) {
  manga::MangaPrefetch worker("/book", 256);
  ASSERT_TRUE(worker.start());
  ASSERT_TRUE(worker.post("/private/tmp/crossink-prefetch-worker-native/page.bmp", 1, -1, true, views));
  worker.stopAndJoin();
  EXPECT_EQ(storage_test::openFiles, 0);
}
TEST_F(MangaPrefetchWorker, BoundedPathFailureDisablesOnlyWarming) {
  manga::MangaPrefetch worker("/book", 1025);
  EXPECT_FALSE(worker.start());
  manga::BitmapPixelInfo info;
  EXPECT_TRUE(manga::probeBitmapPixels("/private/tmp/crossink-prefetch-worker-native/page.bmp", info));
}
TEST_F(MangaPrefetchWorker, RealJpegAndPngUseBorrowedPathAndNeverTouchRenderer) {
  manga::MangaPrefetch worker("/book", 512);
  ASSERT_TRUE(worker.start());
  GfxRenderer::accesses = 0;
  for (const char* suffix : {"/pattern.jpg", "/pattern.png"}) {
    std::string path = std::string(FIXTURE_DIR) + suffix;
    const uint32_t page = suffix[9] == 'j' ? 2 : 3;
    ASSERT_TRUE(worker.post(path.c_str(), page, -1, true, views));
    bool failed = true;
    ASSERT_TRUE(await(worker, failed));
    ASSERT_FALSE(failed);
    EXPECT_EQ(storage_test::openFiles, 0);
    EXPECT_EQ(GfxRenderer::accesses, 0);
    manga::PixelIdentity identity;
    ASSERT_TRUE(manga::MangaPixelCache::sourceIdentity("/book", page, -1, identity));
    manga::MangaPixelCache cache;
    ASSERT_TRUE(cache.configure("/book", page, -1, identity));
    ASSERT_TRUE(cache.open());
    cache.close();
  }
}
manga::MangaPrefetch* cancelOnSyncWorker = nullptr;
TEST_F(MangaPrefetchWorker, CancelDuringFinalSyncCannotPublishAndNextGenerationSucceeds) {
  manga::MangaPrefetch worker("/book", 512);
  ASSERT_TRUE(worker.start());
  cancelOnSyncWorker = &worker;
  storage_test::onSync = [] { cancelOnSyncWorker->cancel(); };
  ASSERT_TRUE(worker.post("/private/tmp/crossink-prefetch-worker-native/page.bmp", 1, -1, true, views));
  bool failed = true;
  ASSERT_TRUE(await(worker, failed));
  EXPECT_FALSE(failed);  // Explicit cancellation must not cause the failure backoff.
  EXPECT_EQ(storage_test::openFiles, 0);
  manga::PixelIdentity identity;
  EXPECT_FALSE(manga::MangaPixelCache::sourceIdentity("/book", 1, -1, identity));
  for (const auto& file : std::filesystem::recursive_directory_iterator(storage_test::root))
    EXPECT_FALSE(file.path().string().ends_with(".tmp"));
  storage_test::onSync = nullptr;
  ASSERT_TRUE(worker.post("/private/tmp/crossink-prefetch-worker-native/page.bmp", 1, -1, true, views));
  ASSERT_TRUE(await(worker, failed));
  EXPECT_FALSE(failed);
  EXPECT_TRUE(manga::MangaPixelCache::sourceIdentity("/book", 1, -1, identity));
}

}  // namespace
