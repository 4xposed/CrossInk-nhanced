#include "BookPreview.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MangaBook.h>
#include <MangaCover.h>
#include <Memory.h>
#include <Xtc.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
bool prepareBookPreview(RecentBook& book, int width, int height, GfxRenderer& renderer,
                        CooperativeCancellation cancellation) {
  if (cancellation.requested()) return false;
  if (book.title.empty()) book.title = book.path.substr(book.path.find_last_of('/') + 1);
  if (!book.coverBmpPath.empty() &&
      Storage.exists(UITheme::getCoverThumbPath(book.coverBmpPath, width, height).c_str()))
    return true;
  if (FsHelpers::hasEpubExtension(book.path)) {
    // EPUB parser state is larger than the render task's stack budget.
    auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("Preview", "Cannot allocate EPUB");
      return false;
    }
    if (!epub->load(true, true, Epub::XLocationLoadMode::Skip)) {
      LOG_ERR("Preview", "Cannot load %s", book.path.c_str());
      return false;
    }
    if (!epub->getTitle().empty()) book.title = epub->getTitle();
    book.author = epub->getAuthor();
    if (cancellation.requested()) return false;
    book.coverBmpPath = epub->getThumbBmpPath();
    if (!epub->hasCoverImage()) {
      book.coverState = RecentBook::CoverState::Missing;
      return false;
    }
    return epub->generateThumbBmp(width, height, &renderer, SETTINGS.getReaderFontId());
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("Preview", "Cannot allocate XTC");
      return false;
    }
    if (!xtc->load()) {
      LOG_ERR("Preview", "Cannot load %s", book.path.c_str());
      return false;
    }
    if (!xtc->getTitle().empty()) book.title = xtc->getTitle();
    book.coverBmpPath = xtc->getThumbBmpPath();
    return !cancellation.requested() && xtc->generateThumbBmp(width, height);
  }
  if (manga::MangaBook::isMangaFolder(book.path.c_str())) {
    auto mangaBook = makeUniqueNoThrow<manga::MangaBook>();
    if (!mangaBook) {
      LOG_ERR("Preview", "Cannot allocate manga");
      return false;
    }
    if (!mangaBook->open(book.path.c_str(), manga::OpenMode::Cover)) {
      LOG_ERR("Preview", "Cannot open manga");
      return false;
    }
    if (!mangaBook->title().empty()) book.title = std::string(mangaBook->title());
    book.coverBmpPath = manga::thumbnailTemplatePath(book.path);
    const auto result = manga::generateThumbnailControlled(*mangaBook, book.path, width, height, cancellation);
    return result == manga::ThumbnailResult::Cached || result == manga::ThumbnailResult::Published;
  }
  return false;
}
void drawBookPreview(const GfxRenderer& renderer, const RecentBook& book, Rect rect) {
  const auto path = UITheme::getCoverThumbPath(book.coverBmpPath, rect.width, rect.height);
  bool drawn = false;
  if (!path.empty() && Storage.exists(path.c_str())) {
    FsFile file;
    if (Storage.openFileForRead("Preview", path, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() && bitmap.getHeight()) {
        const float scale = std::min(float(rect.width) / bitmap.getWidth(), float(rect.height) / bitmap.getHeight());
        const int w = std::max(1, int(bitmap.getWidth() * scale));
        const int h = std::max(1, int(bitmap.getHeight() * scale));
        renderer.drawBitmap(bitmap, rect.x + (rect.width - w) / 2, rect.y + (rect.height - h) / 2, w, h);
        drawn = true;
      }
      if (!file.close()) LOG_ERR("Preview", "Cannot close cover");
    }
  }
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
  if (!drawn) {
    const int inset = std::max(4, rect.width / 8);
    for (int i = 1; i <= 3; ++i)
      renderer.drawLine(rect.x + inset, rect.y + rect.height * i / 5, rect.x + rect.width - inset,
                        rect.y + rect.height * i / 5, true);
  }
}
