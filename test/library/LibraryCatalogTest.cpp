#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>

#include "util/LibraryCatalog.h"
namespace fs = std::filesystem;
int main() {
  storage_test::root = "/tmp/crossink-library-catalog-test";
  fs::remove_all(storage_test::root);
  auto file = [](const char* path) {
    const auto full = storage_test::root + path;
    fs::create_directories(fs::path(full).parent_path());
    std::ofstream(full) << "fixture";
  };
  file("/epubs/a.epub");
  file("/epubs/sub/b.txt");
  file("/epubs-old/c.epub");
  file("/articles/d.md");
  file("/manga/series/book.mki");
  file("/manga/series/page.png");
  file("/anki/deck.anki");
  file("/.crosspoint/cache.epub");
  file("/epubs/ignore.bin");
  auto catalog = std::make_unique<library::LibraryCatalog>();
  auto complete = [&] {
    int steps = 0;
    while (catalog->state() == library::ScanState::Scanning) {
      catalog->step(1);
      assert(++steps < 10000);
    }
    assert(catalog->state() == library::ScanState::Ready);
  };
  assert(catalog->begin("/manga", "/epubs", "/articles"));
  complete();
  assert(catalog->count(library::Category::All) == 5);
  assert(catalog->count(library::Category::Books) == 2);
  assert(catalog->count(library::Category::Manga) == 1);
  assert(catalog->count(library::Category::Articles) == 1);
  library::CatalogEntry entry;
  assert(catalog->entryAt(library::Category::Manga, 0, entry));
  assert(entry.manga && std::string(entry.path) == "/manga/series");
  assert(!catalog->entryAt(library::Category::Manga, 1, entry));
  assert(catalog->begin("/manga", "/epubs/a.epub", "/articles"));
  complete();
  assert(catalog->count(library::Category::Books) == 0);  // A file is not a category directory.
  assert(catalog->begin("/epubs", "/epubs", "/absent"));
  complete();
  assert(catalog->count(library::Category::All) == 5);
  assert(catalog->count(library::Category::Manga) == 2);
  assert(catalog->count(library::Category::Articles) == 0);
  assert(catalog->begin("/manga", "/epubs", "/articles"));
  catalog->step(1);
  catalog->cancel();
  assert(catalog->state() == library::ScanState::Cancelled);
  assert(catalog->begin("/manga", "/epubs", "/articles"));
  storage_test::failWrite = true;
  catalog->step(100);
  assert(catalog->state() == library::ScanState::Failed);
  storage_test::failWrite = false;
  assert(catalog->begin("/manga", "/epubs", "/articles"));
  complete();
  catalog->close();
  assert(catalog->begin("/manga", "/epubs", "/articles"));
  storage_test::failSync = true;
  catalog->step(100);
  assert(catalog->state() == library::ScanState::Failed);
  storage_test::failSync = false;
  assert(catalog->begin("/manga", "/epubs", "/articles"));
  complete();
  storage_test::failRead = true;
  assert(!catalog->entryAt(library::Category::Books, 0, entry));
  assert(catalog->state() == library::ScanState::Failed);
  storage_test::failRead = false;
  assert(storage_test::openFiles == 0);
  file("/.hidden/book.epub");
  assert(catalog->begin("/manga", "/epubs", "/articles", true));
  complete();
  assert(catalog->count(library::Category::All) == 6);
  catalog->close();
  for (int i = 0; i < 1200; ++i) file(("/epubs/large/" + std::to_string(i) + ".epub").c_str());
  assert(catalog->begin("/manga", "/epubs", "/articles"));
  complete();
  assert(catalog->count(library::Category::All) == 1205);
  assert(catalog->entryAt(library::Category::Books, 1201, entry));
  catalog->close();
  assert(storage_test::openFiles == 0);
  fs::remove_all(storage_test::root);
}
