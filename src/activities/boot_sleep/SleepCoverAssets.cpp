#include "SleepCoverAssets.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MangaBook.h>
#include <MangaCover.h>
#include <Memory.h>
#include <Txt.h>
#include <Xtc.h>

#include <cstdint>

#include "CrossPointSettings.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "components/UITheme.h"
#include "components/themes/dashboard/DashboardTheme.h"
#include "components/themes/minimal/MinimalTheme.h"

namespace {

constexpr int kMinimalSleepCoverHeight = MinimalMetrics::homeCoverImageHeight;
constexpr int kMinimalSleepCoverWidth = MinimalMetrics::homeCoverImageWidth;
constexpr int kDashboardSleepCoverHeight = DashboardMetrics::homeCoverImageHeight;
constexpr int kDashboardSleepCoverWidth = DashboardMetrics::homeCoverImageWidth;

bool shouldPrepareFullCover() {
  return SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER ||
         SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM;
}

bool shouldPrepareMinimalCover() {
  return SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::MINIMAL_SLEEP ||
         SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::MINIMAL_STATS_SLEEP;
}

bool shouldPrepareDashboardCover() {
  return SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::DASHBOARD_SLEEP;
}

bool fileExists(const std::string& path) { return !path.empty() && Storage.exists(path.c_str()); }

int readerFontIdForRenderer(const GfxRenderer* renderer) { return renderer ? SETTINGS.getReaderFontId() : 0; }

bool isManga(const std::string& path) { return manga::MangaBook::isMangaFolder(path.c_str()); }

bool mangaCoverCancelled(CooperativeCancellation cancellation, manga::ThumbnailDiagnostics* diagnostics) {
  if (!cancellation.requested()) return false;
  if (diagnostics) diagnostics->result = manga::ThumbnailResult::Cancelled;
  return true;
}

bool prepareManga(const std::string& path, const int width, const int height, CooperativeCancellation cancellation,
                  manga::ThumbnailDiagnostics* diagnostics) {
  if (diagnostics) *diagnostics = {};
  if (mangaCoverCancelled(cancellation, diagnostics)) return false;
  manga::MangaBook book;
  if (!book.open(path.c_str(), manga::OpenMode::Cover, cancellation) ||
      mangaCoverCancelled(cancellation, diagnostics)) {
    mangaCoverCancelled(cancellation, diagnostics);
    return false;
  }
  const auto result = manga::generateThumbnailControlled(book, path, width, height, cancellation, diagnostics);
  LOG_DBG("SLP", "Manga cover result=%d", static_cast<int>(result));
  return result == manga::ThumbnailResult::Cached || result == manga::ThumbnailResult::Published;
}

bool mangaFullDimensions(manga::MangaBook& book, const std::string& path, const GfxRenderer& renderer, int& width,
                         int& height, CooperativeCancellation cancellation) {
  if (cancellation.requested()) return false;
  auto imagePath = makeUniqueNoThrow<char[]>(path.size() + 258);
  if (!imagePath) {
    LOG_ERR("SLP", "OOM for manga cover path (%u bytes)", unsigned(path.size() + 258));
    return false;
  }
  if (book.pageImagePath(0, imagePath.get(), path.size() + 258, cancellation) != manga::PathResult::Found) return false;
  if (cancellation.requested()) return false;
  ImageDimensions source{};
  if (FsHelpers::hasBmpExtension(imagePath.get())) {
    FsFile file;
    if (!Storage.openFileForRead("SLP", imagePath.get(), file)) return false;
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      source.width = bitmap.getWidth();
      source.height = bitmap.getHeight();
    }
    file.close();
  } else {
    const std::string sourcePath(imagePath.get());
    const ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(sourcePath);
    if (!decoder || !decoder->getDimensions(sourcePath, source)) return false;
  }
  // The callback may observe cancellation or a time budget that changed during image decoding.
  // cppcheck-suppress knownConditionTrueFalse
  if (cancellation.requested()) return false;
  return manga::fitThumbnailDimensions(source.width, source.height, renderer.getScreenWidth(),
                                       renderer.getScreenHeight(), width, height);
}

bool prepareFullManga(const std::string& path, const GfxRenderer& renderer, CooperativeCancellation cancellation,
                      manga::ThumbnailDiagnostics* diagnostics, std::string* preparedPath) {
  if (diagnostics) *diagnostics = {};
  if (mangaCoverCancelled(cancellation, diagnostics)) return false;
  manga::MangaBook book;
  int width = 0, height = 0;
  if (!book.open(path.c_str(), manga::OpenMode::Cover, cancellation) ||
      mangaCoverCancelled(cancellation, diagnostics) ||
      !mangaFullDimensions(book, path, renderer, width, height, cancellation) ||
      mangaCoverCancelled(cancellation, diagnostics)) {
    mangaCoverCancelled(cancellation, diagnostics);
    return false;
  }
  const auto result = manga::generateThumbnailControlled(book, path, width, height, cancellation, diagnostics);
  LOG_DBG("SLP", "Full manga cover result=%d", static_cast<int>(result));
  const bool ready = result == manga::ThumbnailResult::Cached || result == manga::ThumbnailResult::Published;
  if (ready && preparedPath) *preparedPath = manga::thumbnailPath(path, width, height);
  return ready;
}

std::string fullMangaPath(const std::string& path, const GfxRenderer& renderer, CooperativeCancellation cancellation) {
  if (cancellation.requested()) return {};
  manga::MangaBook book;
  int width = 0, height = 0;
  if (!book.open(path.c_str(), manga::OpenMode::Cover, cancellation) ||
      !mangaFullDimensions(book, path, renderer, width, height, cancellation))
    return {};
  return manga::thumbnailPath(path, width, height);
}

}  // namespace

namespace SleepCoverAssets {

bool prepareXtc(const Xtc& xtc) {
  bool success = true;
  if (shouldPrepareFullCover()) {
    success = xtc.generateCoverBmp() && success;
  }
  if (shouldPrepareMinimalCover()) {
    success = xtc.generateThumbBmp(static_cast<uint16_t>(kMinimalSleepCoverWidth),
                                   static_cast<uint16_t>(kMinimalSleepCoverHeight)) &&
              success;
  }
  if (shouldPrepareDashboardCover()) {
    success = xtc.generateThumbBmp(static_cast<uint16_t>(kDashboardSleepCoverWidth),
                                   static_cast<uint16_t>(kDashboardSleepCoverHeight)) &&
              success;
  }
  return success;
}

bool prepareTxt(const Txt& txt) {
  if (!shouldPrepareFullCover() && !shouldPrepareMinimalCover() && !shouldPrepareDashboardCover()) {
    return true;
  }
  return txt.generateCoverBmp();
}

bool prepareFullCoverForPath(const std::string& bookPath, const bool cropped, const GfxRenderer* renderer,
                             CooperativeCancellation cancellation, manga::ThumbnailDiagnostics* diagnostics,
                             std::string* preparedPath, bool imageLevels) {
  if (preparedPath) preparedPath->clear();
  if (diagnostics) *diagnostics = {};
  if (mangaCoverCancelled(cancellation, diagnostics)) return false;
  if (bookPath.empty()) {
    return false;
  }

  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    if (!epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true, Epub::XLocationLoadMode::Skip)) {
      return false;
    }
    return epub.generateCoverBmp(cropped, renderer, readerFontIdForRenderer(renderer), imageLevels);
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.crosspoint");
    if (!xtc.load()) {
      return false;
    }
    return xtc.generateCoverBmp();
  }
  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    Txt txt(bookPath, "/.crosspoint");
    return txt.generateCoverBmp(imageLevels);
  }
  if (isManga(bookPath) && renderer) {
    return prepareFullManga(bookPath, *renderer, cancellation, diagnostics, preparedPath);
  }
  return false;
}

bool prepareMinimalCoverForPath(const std::string& bookPath, const GfxRenderer* renderer,
                                CooperativeCancellation cancellation, manga::ThumbnailDiagnostics* diagnostics) {
  if (diagnostics) *diagnostics = {};
  if (bookPath.empty()) {
    return false;
  }

  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    if (!epub.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true, Epub::XLocationLoadMode::Skip)) {
      return false;
    }
    return epub.generateAdaptiveThumbBmp(kMinimalSleepCoverWidth, kMinimalSleepCoverHeight, renderer,
                                         readerFontIdForRenderer(renderer));
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.crosspoint");
    if (!xtc.load()) {
      return false;
    }
    return xtc.generateThumbBmp(static_cast<uint16_t>(kMinimalSleepCoverWidth),
                                static_cast<uint16_t>(kMinimalSleepCoverHeight));
  }
  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    Txt txt(bookPath, "/.crosspoint");
    return txt.generateCoverBmp();
  }
  if (isManga(bookPath)) {
    return prepareManga(bookPath, kMinimalSleepCoverWidth, kMinimalSleepCoverHeight, cancellation, diagnostics);
  }
  return false;
}

bool prepareDashboardCoverForPath(const std::string& bookPath, const GfxRenderer* renderer,
                                  CooperativeCancellation cancellation, manga::ThumbnailDiagnostics* diagnostics) {
  if (diagnostics) *diagnostics = {};
  if (bookPath.empty()) {
    return false;
  }

  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    if (!epub.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true, Epub::XLocationLoadMode::Skip)) {
      return false;
    }
    return epub.generateAdaptiveThumbBmp(kDashboardSleepCoverWidth, kDashboardSleepCoverHeight, renderer,
                                         readerFontIdForRenderer(renderer));
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.crosspoint");
    if (!xtc.load()) {
      return false;
    }
    return xtc.generateThumbBmp(static_cast<uint16_t>(kDashboardSleepCoverWidth),
                                static_cast<uint16_t>(kDashboardSleepCoverHeight));
  }
  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    Txt txt(bookPath, "/.crosspoint");
    return txt.generateCoverBmp();
  }
  if (isManga(bookPath)) {
    return prepareManga(bookPath, kDashboardSleepCoverWidth, kDashboardSleepCoverHeight, cancellation, diagnostics);
  }
  return false;
}

std::string reusableCoverPathFor(const std::string& bookPath) {
  if (FsHelpers::hasEpubExtension(bookPath)) {
    return Epub(bookPath, "/.crosspoint").getThumbBmpPath();
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    return Xtc(bookPath, "/.crosspoint").getThumbBmpPath();
  }
  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    return Txt(bookPath, "/.crosspoint").getCoverBmpPath();
  }
  if (isManga(bookPath)) {
    return manga::thumbnailTemplatePath(bookPath);
  }
  return {};
}

std::string cachedCoverPathFor(const std::string& bookPath, const bool cropped, const GfxRenderer* renderer,
                               CooperativeCancellation cancellation, bool imageLevels) {
  if (cancellation.requested()) return {};
  std::string coverPath;
  if (FsHelpers::hasEpubExtension(bookPath)) {
    coverPath = Epub(bookPath, "/.crosspoint").getCoverBmpPath(cropped, imageLevels);
  } else if (FsHelpers::hasXtcExtension(bookPath)) {
    coverPath = Xtc(bookPath, "/.crosspoint").getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    coverPath = Txt(bookPath, "/.crosspoint").getCoverBmpPath(imageLevels);
  } else if (renderer && isManga(bookPath)) {
    coverPath = fullMangaPath(bookPath, *renderer, cancellation);
  }

  return fileExists(coverPath) ? coverPath : std::string{};
}

std::string cachedMinimalCoverPathFor(const std::string& bookPath) {
  if (FsHelpers::hasEpubExtension(bookPath)) {
    const Epub epub(bookPath, "/.crosspoint");
    const std::string coverPath = epub.getAdaptiveThumbBmpPath(kMinimalSleepCoverWidth, kMinimalSleepCoverHeight);
    return fileExists(coverPath) ? epub.getThumbBmpPath() : std::string{};
  }
  if (isManga(bookPath)) {
    const std::string coverPath = manga::thumbnailPath(bookPath, kMinimalSleepCoverWidth, kMinimalSleepCoverHeight);
    return fileExists(coverPath) ? manga::thumbnailTemplatePath(bookPath) : std::string{};
  }

  const std::string reusablePath = reusableCoverPathFor(bookPath);
  const std::string coverPath =
      UITheme::getCoverThumbPath(reusablePath, kMinimalSleepCoverWidth, kMinimalSleepCoverHeight);
  return fileExists(coverPath) ? reusablePath : std::string{};
}

std::string cachedDashboardCoverPathFor(const std::string& bookPath) {
  if (FsHelpers::hasEpubExtension(bookPath)) {
    const Epub epub(bookPath, "/.crosspoint");
    const std::string coverPath = epub.getAdaptiveThumbBmpPath(kDashboardSleepCoverWidth, kDashboardSleepCoverHeight);
    return fileExists(coverPath) ? epub.getThumbBmpPath() : std::string{};
  }
  if (isManga(bookPath)) {
    const std::string coverPath = manga::thumbnailPath(bookPath, kDashboardSleepCoverWidth, kDashboardSleepCoverHeight);
    return fileExists(coverPath) ? manga::thumbnailTemplatePath(bookPath) : std::string{};
  }

  const std::string reusablePath = reusableCoverPathFor(bookPath);
  const std::string coverPath =
      UITheme::getCoverThumbPath(reusablePath, kDashboardSleepCoverWidth, kDashboardSleepCoverHeight);
  return fileExists(coverPath) ? reusablePath : std::string{};
}

}  // namespace SleepCoverAssets
