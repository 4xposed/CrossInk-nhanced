#include "LibraryFoldersActivity.h"

#include <HalStorage.h>
#include <Memory.h>

#include <cstring>

#include "activities/home/FileBrowserActivity.h"
#include "components/UITheme.h"
#include "util/LibraryPaths.h"
char* LibraryFoldersActivity::folder(int index) const {
  return index == 0   ? SETTINGS.libraryMangaFolder
         : index == 1 ? SETTINGS.libraryBooksFolder
                      : SETTINGS.libraryArticlesFolder;
}
const char* LibraryFoldersActivity::itemValue(int index) const { return folder(index); }
const char* LibraryFoldersActivity::itemLabel(int index) const {
  static constexpr StrId labels[] = {StrId::STR_LIBRARY_MANGA, StrId::STR_LIBRARY_BOOKS, StrId::STR_LIBRARY_ARTICLES};
  return I18N.get(labels[index]);
}
void LibraryFoldersActivity::activate(int index) {
  saveFailed = false;
  auto configured = Storage.open(folder(index));
  const bool available = configured && configured.isDirectory();
  configured.close();
  auto picker = makeUniqueNoThrow<FileBrowserActivity>(renderer, mappedInput, available ? folder(index) : "/",
                                                       FileBrowserActivity::Mode::PickDirectory,
                                                       StrId::STR_SELECT_LIBRARY_FOLDER);
  if (!picker) {
    LOG_ERR("LibraryFolders", "Cannot allocate picker");
    saveFailed = true;
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(picker), [this, index](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto* value = std::get_if<FilePathResult>(&result.data);
    if (!value) return;
    // Two 512-byte paths exceed the small task-stack budget; owned only during save.
    auto scratch = makeUniqueNoThrow<char[]>(library::PATH_CAPACITY * 2);
    if (!scratch || !library::normalizeFolder(value->path, scratch.get(), library::PATH_CAPACITY)) {
      LOG_ERR("LibraryFolders", "Invalid folder or allocation failure");
      saveFailed = true;
      requestUpdate();
      return;
    }
    RenderLock lock(*this);
    char* current = folder(index);
    char* const scratchData = scratch.get();
    char* const previous = scratchData + library::PATH_CAPACITY;
    std::memcpy(previous, current, library::PATH_CAPACITY);
    std::strcpy(current, scratch.get());
    saveFailed = !SETTINGS.saveToFile();
    if (saveFailed) {
      std::memcpy(current, previous, library::PATH_CAPACITY);
      LOG_ERR("LibraryFolders", "Cannot save folder");
    }
    requestUpdate();
  });
}
void LibraryFoldersActivity::drawFeedback() {
  if (saveFailed) GUI.drawPopup(renderer, tr(STR_LIBRARY_FOLDER_ERROR));
}
