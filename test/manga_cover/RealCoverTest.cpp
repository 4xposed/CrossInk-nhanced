#include <Bitmap.h>
#include <CooperativeCancellation.h>
#include <JpegToBmpConverter.h>
#include <PngToBmpConverter.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>
class BufferPrint : public Print {
 public:
  std::vector<uint8_t> bytes;
  bool fail = false;
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* p, size_t n) override {
    if (fail) return 0;
    bytes.insert(bytes.end(), p, p + n);
    return n;
  }
};
static bool convert(const std::string& name, BufferPrint& out, int w, int h, CooperativeCancellation cancel = {}) {
  FsFile in((std::string(COVER_FIXTURES) + "/" + name).c_str());
  bool ok = name.ends_with(".png") ? PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(in, out, w, h, true, cancel)
                                   : JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(in, out, w, h, true, cancel);
  in.close();
  return ok;
}
TEST(RealCover, PreservesPreChangePixelCrcs) {
  std::ifstream f(std::string(COVER_FIXTURES) + "/goldens.txt");
  std::string name, fnv, crc;
  int w, h, ok;
  size_t size, count = 0;
  while (f >> name >> w >> h >> ok >> size >> fnv >> crc) {
    SCOPED_TRACE(name + " " + std::to_string(w) + "x" + std::to_string(h));
    BufferPrint out;
    ASSERT_TRUE(convert(name, out, w, h));
    ASSERT_EQ(out.bytes.size(), size);
    uint32_t sum = 0xffffffffU;
    for (size_t j = 62; j < size; j++) {
      sum ^= out.bytes[j];
      for (int k = 0; k < 8; k++) sum = (sum >> 1) ^ ((0U - (sum & 1)) & 0xedb88320U);
    }
    EXPECT_EQ(~sum, std::stoul(crc, nullptr, 16));
    ++count;
  }
  EXPECT_EQ(count, 50u);
}
TEST(RealCover, RejectsShortPngWrites) {
  BufferPrint out;
  out.fail = true;
  EXPECT_FALSE(convert("rgba.png", out, 123, 180));
}
TEST(RealCover, RejectsShortJpegWrites) {
  BufferPrint out;
  out.fail = true;
  EXPECT_FALSE(convert("baseline.jpg", out, 123, 180));
}
#include <BitmapHelpers.h>

#include <new>
static int allocationIndex = 0, failAllocation = 0;
void* coverTestMalloc(size_t n) {
  if (++allocationIndex == failAllocation) return nullptr;
  return std::malloc(n);
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept {
  if (++allocationIndex == failAllocation) return nullptr;
  return std::malloc(n);
}
void* operator new(size_t n, const std::nothrow_t&) noexcept {
  if (++allocationIndex == failAllocation) return nullptr;
  return std::malloc(n);
}
// Each ditherer keeps all of its error rows in one contiguous allocation.
constexpr int kDitherRowAllocations = 1;
TEST(RealCover, DitherRowsFailWithoutPartialInitialization) {
  for (int failure = 1; failure <= kDitherRowAllocations; failure++) {
    allocationIndex = 0;
    failAllocation = failure;
    Atkinson1BitDitherer dither;
    EXPECT_FALSE(dither.begin(123));
    failAllocation = 0;
    EXPECT_TRUE(dither.begin(123));
    EXPECT_EQ(dither.processPixel(0, 0), 0);
  }
}

TEST(RealCover, CancellationAtEveryCodecBoundaryStopsOutput) {
  for (const char* name : {"baseline.jpg", "progressive.jpg", "rgba.png", "palette.png", "gray.png"}) {
    struct State {
      int polls = 0, stop = 0;
    };
    const auto poll = [](void* p) {
      auto& s = *static_cast<State*>(p);
      return ++s.polls == s.stop;
    };
    State count;
    BufferPrint baseline;
    ASSERT_TRUE(convert(name, baseline, 123, 180, {poll, &count}));
    ASSERT_GT(count.polls, 120);
    for (int stop = 1; stop <= count.polls; stop++) {
      State state{0, stop};
      BufferPrint out;
      EXPECT_FALSE(convert(name, out, 123, 180, {poll, &state})) << name << " boundary " << stop;
      EXPECT_LE(state.polls, stop + 1);
      EXPECT_LE(out.bytes.size(), baseline.bytes.size());
    }
  }
}

#include <MangaBook.h>
#include <MangaCover.h>
#include <unistd.h>
namespace fs = std::filesystem;
class PublishedCover : public ::testing::Test {
 protected:
  fs::path root, folder;
  void SetUp() override {
    char path[] = "/tmp/crossink-real-cover-XXXXXX";
    root = mkdtemp(path);
    folder = root / "book";
    storage_test::root = root;
    fs::create_directories(folder);
    for (const char* name : {"panels.idx", "panels.dat"})
      fs::copy_file(fs::path(MANGA_FIXTURE_DIR) / name, folder / name);
    fs::copy_file(fs::path(COVER_FIXTURES) / "rgba.png", folder / "page_0000.png");
    storage_test::failRenameCall = storage_test::renameCalls = storage_test::failRenameFromCall = 0;
    storage_test::failWriteCall = storage_test::failSyncCall = storage_test::failCloseCall = 0;
  }
  void TearDown() override {
    failAllocation = 0;
    EXPECT_EQ(storage_test::openFiles, 0);
    fs::remove_all(root);
  }
  fs::path output(int w = 123, int h = 180) {
    return storage_test::mapped(manga::thumbnailPath(folder.string(), w, h).c_str());
  }
  std::vector<char> bytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {(std::istreambuf_iterator<char>(f)), {}};
  }
  void changeSource() {
    std::ofstream f(folder / "page_0000.png", std::ios::app | std::ios::binary);
    f.put('x');
  }
};
TEST_F(PublishedCover, ContainedGeometryPublishesAndReuses) {
  manga::MangaBook book;
  ASSERT_TRUE(book.open(folder.c_str()));
  bool changed = false;
  ASSERT_TRUE(manga::generateThumbnail(book, folder.string(), 200, 390, &changed));
  EXPECT_TRUE(changed);
  auto b = bytes(output(200, 390));
  ASSERT_EQ(b.size(), 8462u);
  EXPECT_EQ(static_cast<unsigned char>(b[18]), 200);
  ASSERT_TRUE(manga::generateThumbnail(book, folder.string(), 200, 390, &changed));
  EXPECT_FALSE(changed);
}
TEST_F(PublishedCover, FailedPromotionRetainsPreviousPair) {
  manga::MangaBook book;
  ASSERT_TRUE(book.open(folder.c_str()));
  ASSERT_TRUE(manga::generateThumbnail(book, folder.string(), 123, 180));
  const auto image = bytes(output()), identity = bytes(output().string() + ".src");
  changeSource();
  storage_test::renameCalls = 0;
  storage_test::failRenameCall = 2;
  EXPECT_FALSE(manga::generateThumbnail(book, folder.string(), 123, 180));
  EXPECT_EQ(bytes(output()), image);
  EXPECT_EQ(bytes(output().string() + ".src"), identity);
}
#include <condition_variable>
#include <mutex>
#include <thread>

#include "src/activities/BackgroundSuspension.h"
#include "src/activities/home/MangaCoverWork.h"
TEST(CoverOwner, CancellationIsStickyAcrossWaitingBatchAndFollowingSizes) {
  MangaCoverWork owner;
  owner.authorizeIntent();
  auto stale = owner.batch();
  owner.requestCancellation();
  EXPECT_TRUE(stale.cancelled());
  auto waiting = owner.batch();
  EXPECT_TRUE(waiting.cancelled());
  for (int size = 0; size < 3; size++) EXPECT_TRUE(waiting.cancelled());
  owner.authorizeIntent();
  auto fresh = owner.batch();
  EXPECT_FALSE(fresh.cancelled());
  EXPECT_TRUE(stale.cancelled());
}
#include "src/activities/boot_sleep/SleepCoverBudget.h"
TEST(SleepCover, WholeAttemptExpiryIsStickyAcrossVariantsAndClockWrap) {
  uint32_t now = 0xfffffff0U;
  SleepCoverBudget budget;
  budget.begin([](void* p) { return *static_cast<uint32_t*>(p); }, &now);
  EXPECT_FALSE(budget.cancellation().requested());
  now += 2499;
  EXPECT_FALSE(budget.cancellation().requested());
  now += 1;
  EXPECT_TRUE(budget.cancellation().requested());
  now += 20;
  EXPECT_TRUE(budget.cancellation().requested());
  EXPECT_EQ(budget.elapsed(), 2520u);
  EXPECT_GE(budget.maximumPollGap(), 2499u);
}

TEST_F(PublishedCover, EachReachableAllocationPreservesOldPairAndClosesFiles) {
  for (const char* fixture : {"baseline.jpg", "progressive.jpg", "rgba.png", "palette.png", "gray.png"}) {
    fs::remove(folder / "page_0000.png");
    fs::remove(folder / "page_0000.jpg");
    const auto source = folder / (std::string(fixture).ends_with(".png") ? "page_0000.png" : "page_0000.jpg");
    fs::copy_file(fs::path(COVER_FIXTURES) / fixture, source);
    manga::MangaBook book;
    ASSERT_TRUE(book.open(folder.c_str()));
    ASSERT_TRUE(manga::generateThumbnail(book, folder.string(), 123, 180));
    auto old = bytes(output()), identity = bytes(output().string() + ".src");
    {
      std::ofstream f(source, std::ios::app | std::ios::binary);
      f.put('x');
    }
    // Count every project allocation on the uncached path, aborting before promotion.
    manga::ThumbnailDiagnostics diagnostics;
    auto cancel = [](void* p) {
      return static_cast<manga::ThumbnailDiagnostics*>(p)->stage == manga::ThumbnailStage::Publication;
    };
    allocationIndex = 0;
    EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180, {cancel, &diagnostics}, &diagnostics),
              manga::ThumbnailResult::Cancelled);
    const int count = allocationIndex;
    ASSERT_GE(count, 6) << fixture;
    for (int failure = 1; failure <= count; failure++) {
      SCOPED_TRACE(std::string(fixture) + " allocation " + std::to_string(failure));
      allocationIndex = 0;
      failAllocation = failure;
      auto result = manga::generateThumbnailControlled(book, folder.string(), 123, 180);
      failAllocation = 0;
      EXPECT_EQ(result, manga::ThumbnailResult::Failed);
      EXPECT_EQ(bytes(output()), old);
      EXPECT_EQ(bytes(output().string() + ".src"), identity);
      EXPECT_EQ(storage_test::openFiles, 0);
      EXPECT_FALSE(fs::exists(output().string() + ".tmp"));
      EXPECT_FALSE(fs::exists(output().string() + ".src.tmp"));
    }
  }
}
TEST_F(PublishedCover, FinalizationFaultMatrixPreservesOldPair) {
  manga::MangaBook book;
  ASSERT_TRUE(book.open(folder.c_str()));
  ASSERT_TRUE(manga::generateThumbnail(book, folder.string(), 123, 180));
  const auto old = bytes(output()), identity = bytes(output().string() + ".src");
  changeSource();
  // Source validation/temporary validation also close files; those failures must not publish.
  for (int operation = 0; operation < 4; operation++) {
    const int failures = operation == 0 ? 243 : (operation == 1 ? 2 : (operation == 2 ? 20 : 4));
    for (int failure = 1; failure <= failures; failure++) {
      {
        std::ofstream f(output(), std::ios::binary | std::ios::trunc);
        f.write(old.data(), old.size());
      }
      {
        std::ofstream f(output().string() + ".src", std::ios::binary | std::ios::trunc);
        f.write(identity.data(), identity.size());
      }
      storage_test::writeCalls = storage_test::syncCalls = storage_test::closeCalls = storage_test::renameCalls = 0;
      int* fault = operation == 0   ? &storage_test::failWriteCall
                   : operation == 1 ? &storage_test::failSyncCall
                   : operation == 2 ? &storage_test::failCloseCall
                                    : &storage_test::failRenameCall;
      *fault = failure;
      auto result = manga::generateThumbnailControlled(book, folder.string(), 123, 180);
      const int calls = operation == 0   ? storage_test::writeCalls
                        : operation == 1 ? storage_test::syncCalls
                        : operation == 2 ? storage_test::closeCalls
                                         : storage_test::renameCalls;
      *fault = 0;
      if (calls < failure) break;
      EXPECT_EQ(result, manga::ThumbnailResult::Failed) << operation << "/" << failure;
      EXPECT_EQ(bytes(output()), old) << operation << "/" << failure;
      EXPECT_EQ(bytes(output().string() + ".src"), identity) << operation << "/" << failure;
      EXPECT_EQ(storage_test::openFiles, 0);
      EXPECT_FALSE(fs::exists(output().string() + ".tmp"));
      EXPECT_FALSE(fs::exists(output().string() + ".src.tmp"));
    }
  }
}
TEST_F(PublishedCover, CancellationAtEveryCoverBoundaryPreservesPair) {
  manga::MangaBook book;
  ASSERT_TRUE(book.open(folder.c_str()));
  ASSERT_TRUE(manga::generateThumbnail(book, folder.string(), 123, 180));
  const auto old = bytes(output()), identity = bytes(output().string() + ".src");
  changeSource();
  struct State {
    int polls = 0, stop = 0;
  };
  const auto poll = [](void* p) {
    auto& s = *static_cast<State*>(p);
    return ++s.polls == s.stop;
  };
  manga::ThumbnailDiagnostics d;
  int count = 0;
  struct Count {
    int& n;
    manga::ThumbnailDiagnostics& d;
  };
  Count c{count, d};
  const auto last = [](void* p) {
    auto& c = *static_cast<Count*>(p);
    ++c.n;
    return c.d.stage == manga::ThumbnailStage::Publication;
  };
  EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180, {last, &c}, &d),
            manga::ThumbnailResult::Cancelled);
  for (int boundary = 1; boundary <= count; boundary++) {
    State state{0, boundary};
    EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180, {poll, &state}),
              manga::ThumbnailResult::Cancelled)
        << boundary;
    EXPECT_EQ(state.polls, boundary);
    EXPECT_EQ(storage_test::openFiles, 0);
    EXPECT_EQ(bytes(output()), old);
    EXPECT_EQ(bytes(output().string() + ".src"), identity);
  }
}
namespace {
std::mutex ownerMutex;
struct OwnerLock {
  std::unique_lock<std::mutex> lock{ownerMutex};
};
}  // namespace
TEST_F(PublishedCover, RealConversionDrainsBeforeNavigationSleepAndTransferGate) {
  for (int transition = 0; transition < 12; transition++) {
    struct Owner {
      MangaCoverWork work;
      void requestBackgroundCancellation() { work.requestCancellation(); }
      bool prepareToSuspend() { return !work.active && storage_test::openFiles == 0; }
    } owner;
    owner.work.authorizeIntent();
    std::atomic<bool> running{false};
    std::thread render([&] {
      std::unique_lock lock(ownerMutex);
      owner.work.active = true;
      auto batch = owner.work.batch();
      manga::MangaBook book;
      EXPECT_TRUE(book.open(folder.c_str()));
      manga::ThumbnailDiagnostics diagnostics;
      struct Context {
        MangaCoverWork::Batch& batch;
        std::atomic<bool>& running;
        manga::ThumbnailDiagnostics& diagnostics;
      };
      Context context{batch, running, diagnostics};
      auto callback = [](void* p) {
        auto& c = *static_cast<Context*>(p);
        if (c.diagnostics.stage != manga::ThumbnailStage::Crc) return false;
        c.running.store(true);
        while (!c.batch.cancelled()) std::this_thread::yield();
        return true;
      };
      EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180, {callback, &context}, &diagnostics),
                manga::ThumbnailResult::Cancelled);
      owner.work.active = false;
    });
    while (!running.load()) std::this_thread::yield();
    EXPECT_TRUE(prepareBackgroundSuspension<OwnerLock>(&owner));
    render.join();
    EXPECT_EQ(storage_test::openFiles, 0);
  }
}

TEST_F(PublishedCover, CompleteBackupRecoversWithoutLosingOldPixels) {
  manga::MangaBook book;
  ASSERT_TRUE(book.open(folder.c_str()));
  ASSERT_TRUE(manga::generateThumbnail(book, folder.string(), 123, 180));
  auto image = bytes(output()), identity = bytes(output().string() + ".src");
  fs::rename(output(), output().string() + ".bak");
  fs::rename(output().string() + ".src", output().string() + ".src.bak");
  EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180), manga::ThumbnailResult::Cached);
  EXPECT_EQ(bytes(output()), image);
  EXPECT_EQ(bytes(output().string() + ".src"), identity);
}
TEST_F(PublishedCover, SleepExpiresDuringSourceCrcAndSuppressesFollowingVariant) {
  uint32_t now = 100;
  SleepCoverBudget budget;
  budget.begin([](void* p) { return *static_cast<uint32_t*>(p); }, &now);
  auto cancellation = budget.cancellation();
  manga::ThumbnailDiagnostics diagnostics;
  struct Context {
    CooperativeCancellation cancel;
    uint32_t& now;
    manga::ThumbnailDiagnostics& diagnostics;
  };
  Context context{cancellation, now, diagnostics};
  auto poll = [](void* p) {
    auto& c = *static_cast<Context*>(p);
    if (c.diagnostics.stage == manga::ThumbnailStage::Crc) c.now += 500;
    return c.cancel.requested();
  };
  manga::MangaBook book;
  ASSERT_TRUE(book.open(folder.c_str()));
  EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180, {poll, &context}, &diagnostics),
            manga::ThumbnailResult::Cancelled);
  EXPECT_EQ(diagnostics.stage, manga::ThumbnailStage::Crc);
  EXPECT_EQ(budget.elapsed(), 2500u);
  EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 296, 444, budget.cancellation()),
            manga::ThumbnailResult::Cancelled);
  EXPECT_EQ(storage_test::openFiles, 0);
  EXPECT_FALSE(fs::exists(output()));
  EXPECT_FALSE(fs::exists(output(296, 444)));
}
TEST_F(PublishedCover, OversizedOrInconsistentBmpCacheIsRejected) {
  manga::MangaBook book;
  ASSERT_TRUE(book.open(folder.c_str()));
  ASSERT_TRUE(manga::generateThumbnail(book, folder.string(), 123, 180));
  for (uint32_t badWidth : {0u, 124u, 0x7fffffffu}) {
    std::fstream f(output(), std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(18);
    f.write(reinterpret_cast<char*>(&badWidth), 4);
    f.close();
    EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180), manga::ThumbnailResult::Published);
  }
}
TEST_F(PublishedCover, BitmapCancellationIncludesFinalOutputRowAndNoDitherAllocation) {
  fs::remove(folder / "page_0000.png");
  fs::copy_file(fs::path(COVER_FIXTURES) / "mono.bmp", folder / "page_0000.bmp");
  manga::MangaBook book;
  ASSERT_TRUE(book.open(folder.c_str()));
  ASSERT_TRUE(manga::generateThumbnail(book, folder.string(), 123, 180));
  const auto old = bytes(output()), identity = bytes(output().string() + ".src");
  {
    std::ofstream f(folder / "page_0000.bmp", std::ios::binary | std::ios::app);
    f.put('x');
  }
  manga::ThumbnailDiagnostics diagnostics;
  int count = 0;
  struct State {
    int polls = 0, stop = 0;
  };
  const auto callback = [](void* p) {
    auto& s = *static_cast<State*>(p);
    return ++s.polls == s.stop;
  };
  struct Count {
    int& count;
    manga::ThumbnailDiagnostics& diagnostics;
  };
  Count c{count, diagnostics};
  const auto last = [](void* p) {
    auto& c = *static_cast<Count*>(p);
    ++c.count;
    return c.diagnostics.stage == manga::ThumbnailStage::Publication;
  };
  EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180, {last, &c}, &diagnostics),
            manga::ThumbnailResult::Cancelled);
  for (int stop = 1; stop <= count; stop++) {
    State state{0, stop};
    EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180, {callback, &state}),
              manga::ThumbnailResult::Cancelled);
    EXPECT_EQ(bytes(output()), old);
    EXPECT_EQ(bytes(output().string() + ".src"), identity);
    EXPECT_EQ(storage_test::openFiles, 0);
  }
}
TEST(RealCover, AllDitherClassesRejectEveryInnerAllocation) {
  for (int failure = 1; failure <= kDitherRowAllocations; failure++) {
    allocationIndex = 0;
    failAllocation = failure;
    AtkinsonDitherer d;
    EXPECT_FALSE(d.begin(123));
    failAllocation = 0;
    EXPECT_TRUE(d.begin(123));
  }
  for (int failure = 1; failure <= kDitherRowAllocations; failure++) {
    allocationIndex = 0;
    failAllocation = failure;
    FloydSteinbergDitherer d;
    EXPECT_FALSE(d.begin(123));
    failAllocation = 0;
    EXPECT_TRUE(d.begin(123));
  }
}

TEST(CoverOwner, MainWriterPublishesEveryCancellationWithoutResettingTheBatch) {
  MangaCoverWork owner;
  owner.authorizeIntent();
  auto initial = owner.batch();
  uint32_t previous = owner.requestCancellation();
  for (int i = 0; i < 1000; ++i) {
    const uint32_t next = owner.requestCancellation();
    EXPECT_EQ(next, previous + 1);
    EXPECT_TRUE(initial.cancelled());
    owner.authorizeIntent(next);
    auto current = owner.batch();
    EXPECT_FALSE(current.cancelled());
    previous = next;
  }
}

TEST_F(PublishedCover, AspectInconsistentCompleteBmpIsNotCached) {
  manga::MangaBook book;
  ASSERT_TRUE(book.open(folder.c_str()));
  ASSERT_TRUE(manga::generateThumbnail(book, folder.string(), 123, 180));
  for (auto dimensions : {std::pair<int, int>{123, 1}, {1, 180}}) {
    BmpHeader header;
    createBmpHeader(&header, dimensions.first, dimensions.second, BmpRowOrder::TopDown);
    std::vector<char> malformed(header.fileHeader.bfSize, 0);
    memcpy(malformed.data(), &header, sizeof(header));
    std::ofstream f(output(), std::ios::binary | std::ios::trunc);
    f.write(malformed.data(), malformed.size());
    f.close();
    EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180), manga::ThumbnailResult::Published);
  }
}
TEST_F(PublishedCover, SecondBackupAndRollbackFailureRecoversCrossedOldPair) {
  manga::MangaBook book;
  ASSERT_TRUE(book.open(folder.c_str()));
  ASSERT_TRUE(manga::generateThumbnail(book, folder.string(), 123, 180));
  auto image = bytes(output()), identity = bytes(output().string() + ".src");
  changeSource();
  storage_test::renameCalls = 0;
  storage_test::failRenameFromCall = 2;
  ASSERT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180), manga::ThumbnailResult::Failed);
  storage_test::failRenameFromCall = 0;
  ASSERT_FALSE(fs::exists(output()));
  ASSERT_EQ(bytes(output().string() + ".bak"), image);
  ASSERT_EQ(bytes(output().string() + ".src"), identity);
  // Reopen after a failed transaction, then stop before fresh source conversion.
  manga::MangaBook reopened;
  ASSERT_TRUE(reopened.open(folder.c_str()));
  manga::ThumbnailDiagnostics diagnostics;
  auto cancel = [](void* p) {
    return static_cast<manga::ThumbnailDiagnostics*>(p)->stage == manga::ThumbnailStage::Crc;
  };
  EXPECT_EQ(
      manga::generateThumbnailControlled(reopened, folder.string(), 123, 180, {cancel, &diagnostics}, &diagnostics),
      manga::ThumbnailResult::Cancelled);
  EXPECT_EQ(bytes(output()), image);
  EXPECT_EQ(bytes(output().string() + ".src"), identity);
}

TEST_F(PublishedCover, CrossedPairsRequireMatchingBmpDigestInBothDirections) {
  manga::MangaBook book;
  ASSERT_TRUE(book.open(folder.c_str()));
  ASSERT_TRUE(manga::generateThumbnail(book, folder.string(), 123, 180));
  const auto oldImage = bytes(output()), oldIdentity = bytes(output().string() + ".src");
  fs::copy_file(fs::path(COVER_FIXTURES) / "gray.png", folder / "page_0000.png", fs::copy_options::overwrite_existing);
  ASSERT_TRUE(manga::generateThumbnail(book, folder.string(), 123, 180));
  const auto newImage = bytes(output()), newIdentity = bytes(output().string() + ".src");
  ASSERT_NE(oldImage, newImage);
  auto write = [](const fs::path& path, const std::vector<char>& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(data.data(), data.size());
  };
  for (bool backupImage : {true, false}) {
    const auto imagePath = output().string() + (backupImage ? ".bak" : "");
    const auto idPath = output().string() + (backupImage ? ".src" : ".src.bak");
    fs::remove(output());
    fs::remove(output().string() + ".src");
    write(imagePath, oldImage);
    write(idPath, newIdentity);
    manga::ThumbnailDiagnostics diagnostics;
    auto cancel = [](void* p) {
      return static_cast<manga::ThumbnailDiagnostics*>(p)->stage == manga::ThumbnailStage::Crc;
    };
    EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180, {cancel, &diagnostics}, &diagnostics),
              manga::ThumbnailResult::Cancelled);
    EXPECT_EQ(bytes(imagePath), oldImage);
    EXPECT_EQ(bytes(idPath), newIdentity);
    EXPECT_FALSE(fs::exists(output().string() + (backupImage ? "" : ".src")));
    // The matching identity makes precisely the same crossed candidate recoverable.
    write(idPath, oldIdentity);
    EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180, {cancel, &diagnostics}, &diagnostics),
              manga::ThumbnailResult::Cancelled);
    EXPECT_EQ(bytes(output()), oldImage);
    EXPECT_EQ(bytes(output().string() + ".src"), oldIdentity);
  }
}
TEST_F(PublishedCover, RecoveryCancellationAtEveryDigestBoundaryLeavesCandidateUntouched) {
  manga::MangaBook book;
  ASSERT_TRUE(book.open(folder.c_str()));
  ASSERT_TRUE(manga::generateThumbnail(book, folder.string(), 123, 180));
  const auto image = bytes(output()), identity = bytes(output().string() + ".src");
  fs::rename(output(), output().string() + ".bak");
  manga::ThumbnailDiagnostics diagnostics;
  struct Context {
    manga::ThumbnailDiagnostics& diagnostics;
    int polls = 0, stop = 0;
  } context{diagnostics};
  auto poll = [](void* p) {
    auto& c = *static_cast<Context*>(p);
    if (c.diagnostics.stage != manga::ThumbnailStage::Validation) return true;
    return ++c.polls == c.stop;
  };
  EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180, {poll, &context}, &diagnostics),
            manga::ThumbnailResult::Cancelled);
  const int count = context.polls;
  ASSERT_GT(count, 10);
  for (int stop = 1; stop <= count; ++stop) {
    if (fs::exists(output())) fs::rename(output(), output().string() + ".bak");
    context.polls = 0;
    context.stop = stop;
    EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180, {poll, &context}, &diagnostics),
              manga::ThumbnailResult::Cancelled);
    // The final poll is after the short masked recovery; either complete location is safe.
    EXPECT_EQ(fs::exists(output()) ? bytes(output()) : bytes(output().string() + ".bak"), image);
    EXPECT_EQ(bytes(output().string() + ".src"), identity);
  }
}
TEST_F(PublishedCover, DiagnosticsDistinguishEveryResult) {
  manga::MangaBook book;
  ASSERT_TRUE(book.open(folder.c_str()));
  manga::ThumbnailDiagnostics diagnostics;
  EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180, {}, &diagnostics),
            manga::ThumbnailResult::Published);
  EXPECT_EQ(diagnostics.result, manga::ThumbnailResult::Published);
  EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180, {}, &diagnostics),
            manga::ThumbnailResult::Cached);
  EXPECT_EQ(diagnostics.result, manga::ThumbnailResult::Cached);
  auto cancel = [](void*) { return true; };
  EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180, {cancel, nullptr}, &diagnostics),
            manga::ThumbnailResult::Cancelled);
  EXPECT_EQ(diagnostics.result, manga::ThumbnailResult::Cancelled);
  EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 801, 180, {}, &diagnostics),
            manga::ThumbnailResult::Failed);
  EXPECT_EQ(diagnostics.result, manga::ThumbnailResult::Failed);
}

TEST_F(PublishedCover, EveryGoldenCodecGeometryPublishesAndReusesWithMcg3Identity) {
  std::ifstream f(std::string(COVER_FIXTURES) + "/goldens.txt");
  std::string name, fnv, crc;
  int w, h, ok;
  size_t size, count = 0;
  auto u32 = [](const std::vector<char>& bytes, size_t offset) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= uint32_t(static_cast<uint8_t>(bytes[offset + i])) << (i * 8);
    return value;
  };
  while (f >> name >> w >> h >> ok >> size >> fnv >> crc) {
    SCOPED_TRACE(name + " " + std::to_string(w) + "x" + std::to_string(h));
    fs::remove(folder / "page_0000.png");
    fs::remove(folder / "page_0000.jpg");
    fs::copy_file(fs::path(COVER_FIXTURES) / name,
                  folder / (name.ends_with(".png") ? "page_0000.png" : "page_0000.jpg"));
    manga::MangaBook book;
    ASSERT_TRUE(book.open(folder.c_str()));
    ASSERT_EQ(manga::generateThumbnailControlled(book, folder.string(), w, h), manga::ThumbnailResult::Published);
    auto image = bytes(output(w, h)), identity = bytes(output(w, h).string() + ".src");
    ASSERT_EQ(image.size(), size);
    ASSERT_EQ(identity.size(), 40u);
    EXPECT_EQ(std::string(identity.data(), 4), "MCG3");
    EXPECT_EQ(u32(identity, 12), unsigned(w));
    EXPECT_EQ(u32(identity, 16), unsigned(h));
    EXPECT_EQ(u32(identity, 20), u32(image, 18));
    EXPECT_EQ(u32(identity, 24), unsigned(std::abs(static_cast<int32_t>(u32(image, 22)))));
    uint32_t sum = 0xffffffffU;
    for (size_t j = 62; j < image.size(); ++j) {
      sum ^= static_cast<uint8_t>(image[j]);
      for (int k = 0; k < 8; ++k) sum = (sum >> 1) ^ ((0U - (sum & 1)) & 0xedb88320U);
    }
    EXPECT_EQ(~sum, std::stoul(crc, nullptr, 16));
    EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), w, h), manga::ThumbnailResult::Cached);
    ++count;
  }
  EXPECT_EQ(count, 50u);
}
TEST_F(PublishedCover, SameExtentPixelCorruptionRegenerates) {
  manga::MangaBook book;
  ASSERT_TRUE(book.open(folder.c_str()));
  ASSERT_TRUE(manga::generateThumbnail(book, folder.string(), 123, 180));
  const auto original = bytes(output());
  auto corrupt = original;
  corrupt.back() ^= 1;
  std::ofstream f(output(), std::ios::binary | std::ios::trunc);
  f.write(corrupt.data(), corrupt.size());
  f.close();
  EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180), manga::ThumbnailResult::Published);
  EXPECT_EQ(bytes(output()), original);
}

TEST_F(PublishedCover, BackupRecoveryRenameFailuresRemainRecoverable) {
  manga::MangaBook book;
  ASSERT_TRUE(book.open(folder.c_str()));
  ASSERT_TRUE(manga::generateThumbnail(book, folder.string(), 123, 180));
  const auto image = bytes(output()), identity = bytes(output().string() + ".src");
  for (int failure : {1, 2}) {
    fs::rename(output(), output().string() + ".bak");
    fs::rename(output().string() + ".src", output().string() + ".src.bak");
    storage_test::renameCalls = 0;
    storage_test::failRenameFromCall = failure;
    EXPECT_EQ(manga::generateThumbnailControlled(book, folder.string(), 123, 180), manga::ThumbnailResult::Failed);
    storage_test::failRenameFromCall = 0;
    manga::MangaBook reopened;
    ASSERT_TRUE(reopened.open(folder.c_str()));
    manga::ThumbnailDiagnostics diagnostics;
    auto cancel = [](void* p) {
      return static_cast<manga::ThumbnailDiagnostics*>(p)->stage == manga::ThumbnailStage::Crc;
    };
    EXPECT_EQ(
        manga::generateThumbnailControlled(reopened, folder.string(), 123, 180, {cancel, &diagnostics}, &diagnostics),
        manga::ThumbnailResult::Cancelled);
    EXPECT_EQ(bytes(output()), image);
    EXPECT_EQ(bytes(output().string() + ".src"), identity);
  }
}
