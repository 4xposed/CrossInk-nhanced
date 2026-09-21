#pragma once
#include <HalStorage.h>

#include <array>

#include "LibraryPaths.h"
namespace library {
enum class ScanState : uint8_t { Idle, Scanning, Ready, Failed, Cancelled };
struct CatalogEntry {
  char path[PATH_CAPACITY]{};
  bool manga = false;
};
// Owned by LibraryActivity on the heap. All paths/queue storage are bounded;
// the unbounded directory queue and category offsets live on SD.
class LibraryCatalog {
 public:
  ~LibraryCatalog() { close(); }
  bool begin(const char* mangaFolder, const char* booksFolder, const char* articlesFolder, bool showHidden = false);
  ScanState step(size_t entryBudget);
  void cancel();
  void close();
  ScanState state() const { return status; }
  uint32_t count(Category category) const { return counts[static_cast<size_t>(category)]; }
  bool entryAt(Category category, uint32_t index, CatalogEntry& out);

 private:
  ScanState status = ScanState::Idle;
  FsFile queue, records, directory;
  std::array<FsFile, 4> offsets;
  std::array<uint32_t, 4> counts{};
  char folders[3][PATH_CAPACITY]{};
  char currentDirectory[PATH_CAPACITY]{};
  char path[PATH_CAPACITY]{};
  char name[PATH_CAPACITY]{};
  bool showHidden = false;
  std::array<bool, 3> categoryDirectories{};
  uint32_t queueRead = 0, queueEnd = 0;
  bool enqueue(const char* value);
  bool append(bool manga);
  ScanState fail(const char* operation);
};
}  // namespace library
