#include <BookmarkStore.h>
#include <HalStorage.h>
#include <unistd.h>
#include <uzlib.h>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
int main(int argc, char** argv) {
  char temp[] = "/tmp/crossink-bookmark10e-XXXXXX";
  storage_test::root = mkdtemp(temp);
  std::filesystem::create_directories(storage_test::root + "/.crosspoint/bookmarks");
  constexpr const char* book = "/manga";
  const auto file =
      storage_test::root + "/.crosspoint/bookmarks/manga_" + std::to_string(uzlib_crc32(book, 6, 0)) + ".bin";
  BookmarkStore store;
  assert(store.loadForBook(book, "Title", "Author", "manga"));
  const std::string mode = argc > 1 ? argv[1] : "write";
  if (mode == "remove") {
    store.addBookmark(0, 0, 1, "Page");
    std::filesystem::rename(file, file + ".parked");
    std::filesystem::create_directory(file);
    {
      std::ofstream marker(file + "/blocked");
      marker << "blocked";
    }
    assert(store.removeBookmarkAt(0));
    assert(!store.saveToFileChecked());
    std::filesystem::remove_all(file);
    std::filesystem::rename(file + ".parked", file);
    assert(store.saveToFileChecked());
    assert(!std::filesystem::exists(file));
  } else {
    storage_test::failWrite = mode == "write";
    storage_test::failSync = mode == "sync";
    if (mode == "close") storage_test::failCloseCall = storage_test::closeCalls + 1;
    store.addBookmark(0, 0, 1, "Page");
    if (mode == "close") storage_test::failCloseCall = storage_test::closeCalls + 1;
    assert(!store.saveToFileChecked());
    storage_test::failWrite = storage_test::failSync = false;
    storage_test::failCloseCall = 0;
    assert(store.saveToFileChecked());
    store.unload();
    assert(store.loadForBook(book, "Title", "Author", "manga"));
    assert(store.getBookmarks().size() == 1);
  }
  store.unload();
  assert(storage_test::openFiles == 0);
  std::filesystem::remove_all(storage_test::root);
  puts("Bookmark failed save/removal retains retry state");
}
