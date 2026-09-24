#include "LibraryCatalog.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <MangaBook.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
namespace library {
namespace {
constexpr const char* ROOT = "/.crosspoint/library";
constexpr const char* RECORDS = "/.crosspoint/library/records.bin";
constexpr const char* QUEUE = "/.crosspoint/library/queue.bin";
constexpr const char* OFFSETS[] = {"/.crosspoint/library/all.idx", "/.crosspoint/library/manga.idx",
                                   "/.crosspoint/library/books.idx", "/.crosspoint/library/articles.idx"};
bool put32(FsFile& file, uint32_t value) {
  const uint8_t bytes[] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
  return file.write(bytes, 4) == 4;
}
bool get32(FsFile& file, uint32_t& value) {
  uint8_t bytes[4];
  if (file.read(bytes, 4) != 4) return false;
  value = uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
  return true;
}
bool supported(const char* path) {
  return FsHelpers::hasEpubExtension(std::string_view(path)) || FsHelpers::hasTxtExtension(std::string_view(path)) ||
         FsHelpers::hasMarkdownExtension(path) || FsHelpers::hasXtcExtension(path);
}
bool excluded(const char* name, bool showHidden) {
  // Application/cache and OS metadata trees never belong in the reading gallery.
  return !std::strcmp(name, ".") || !std::strcmp(name, "..") || !std::strcmp(name, ".crosspoint") ||
         !std::strncmp(name, ".crossink", 9) || !std::strncmp(name, "._", 2) || !std::strcmp(name, ".DS_Store") ||
         !std::strcmp(name, ".Trashes") || !std::strcmp(name, ".Spotlight-V100") || (!showHidden && name[0] == '.') ||
         !std::strcmp(name, "System Volume Information") || !std::strcmp(name, "$RECYCLE.BIN");
}
}  // namespace
void LibraryCatalog::close() {
  directory.close();
  queue.close();
  records.close();
  for (auto& file : offsets) file.close();
  status = ScanState::Idle;
}
ScanState LibraryCatalog::fail(const char* operation) {
  LOG_ERR("Library", "Catalog %s failed", operation);
  close();
  counts.fill(0);
  Storage.remove(QUEUE);
  Storage.remove(RECORDS);
  for (const auto* p : OFFSETS) Storage.remove(p);
  status = ScanState::Failed;
  return status;
}
void LibraryCatalog::cancel() {
  close();
  counts.fill(0);
  Storage.remove(QUEUE);
  Storage.remove(RECORDS);
  for (const auto* p : OFFSETS) Storage.remove(p);
  status = ScanState::Cancelled;
}
bool LibraryCatalog::enqueue(const char* value) {
  const size_t length = std::strlen(value);
  if (length >= PATH_CAPACITY || queueEnd > UINT32_MAX - length - 4) return false;
  if (!queue.seek(queueEnd) || !put32(queue, length) || queue.write(value, length) != length) return false;
  queueEnd += 4 + length;
  return true;
}
bool LibraryCatalog::begin(const char* mangaFolder, const char* booksFolder, const char* articlesFolder,
                           bool showHidden) {
  this->showHidden = showHidden;
  close();
  counts.fill(0);
  queueRead = queueEnd = 0;
  const char* values[] = {mangaFolder, booksFolder, articlesFolder};
  for (size_t i = 0; i < 3; ++i) {
    if (!normalizeFolder(values[i], folders[i], PATH_CAPACITY)) {
      fail("folder validation");
      return false;
    }
  }
  for (size_t i = 0; i < 3; ++i) {
    categoryDirectories[i] = false;
    if (!Storage.exists(folders[i])) continue;
    auto folder = Storage.open(folders[i]);
    if (!folder) {
      fail("category open");
      return false;
    }
    categoryDirectories[i] = folder.isDirectory();
    if (!folder.close()) {
      fail("category close");
      return false;
    }
  }
  if (!Storage.ensureDirectoryExists(ROOT)) {
    fail("directory creation");
    return false;
  }
  queue = Storage.open(QUEUE, O_RDWR | O_CREAT | O_TRUNC);
  records = Storage.open(RECORDS, O_RDWR | O_CREAT | O_TRUNC);
  for (size_t i = 0; i < 4; ++i) offsets[i] = Storage.open(OFFSETS[i], O_RDWR | O_CREAT | O_TRUNC);
  if (!queue || !records) {
    fail("open");
    return false;
  }
  if (std::any_of(offsets.begin(), offsets.end(), [](const auto& file) { return !file; })) {
    fail("offset open");
    return false;
  }
  if (records.write("CLIB\1", 5) != 5 || !enqueue("/")) {
    fail("initial write");
    return false;
  }
  status = ScanState::Scanning;
  return true;
}
bool LibraryCatalog::append(bool manga) {
  const size_t length = std::strlen(path);
  const auto position = records.position();
  if (position > UINT32_MAX - length - 4) return false;
  uint8_t mask = 1;
  for (size_t i = 0; i < 3; ++i)
    if (categoryDirectories[i] && containsPath(folders[i], path)) mask |= 1 << (i + 1);
  const uint8_t header[] = {uint8_t(length), uint8_t(length >> 8), uint8_t(manga), mask};
  if (records.write(header, 4) != 4 || records.write(path, length) != length) return false;
  for (size_t i = 0; i < 4; ++i)
    if (mask & (1 << i)) {
      if (counts[i] >= UINT32_MAX / 4 || !put32(offsets[i], position)) return false;
      ++counts[i];
    }
  return true;
}
ScanState LibraryCatalog::step(size_t entryBudget) {
  if (status != ScanState::Scanning) return status;
  while (entryBudget--) {
    if (!directory) {
      if (queueRead == queueEnd) {
        if (!queue.sync() || !queue.close() || !records.sync()) return fail("sync");
        for (auto& file : offsets)
          if (!file.sync()) return fail("offset sync");
        status = ScanState::Ready;
        return status;
      }
      uint32_t length = 0;
      if (!queue.seek(queueRead) || !get32(queue, length) || !length || length >= PATH_CAPACITY ||
          queue.read(currentDirectory, length) != static_cast<int>(length))
        return fail("queue read");
      currentDirectory[length] = 0;
      queueRead += length + 4;
      directory = Storage.open(currentDirectory);
      if (!directory || !directory.isDirectory()) return fail("directory open");
    }
    auto entry = directory.openNextFileChecked();
    if (!entry) {
      if (directory.enumerationFailed() || entry.allocationFailed()) return fail("enumeration");
      if (!directory.close()) return fail("directory close");
      continue;
    }
    const size_t length = entry.getName(name, sizeof(name));
    const bool isDirectory = entry.isDirectory();
    if (!entry.close()) return fail("entry close");
    if (!length || length >= sizeof(name) - 1) return fail("entry name");
    if (excluded(name, showHidden)) continue;
    const int written = std::snprintf(path, sizeof(path), "%s%s%s", currentDirectory,
                                      std::strcmp(currentDirectory, "/") ? "/" : "", name);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(path)) return fail("path length");
    if (isDirectory) {
      if (manga::MangaBook::isMangaFolder(path)) {
        if (!append(true)) return fail("manga write");
      } else if (!enqueue(path))
        return fail("queue write");
    } else if (supported(path) && !append(false))
      return fail("record write");
  }
  return status;
}
bool LibraryCatalog::entryAt(Category category, uint32_t index, CatalogEntry& out) {
  if (status != ScanState::Ready || index >= count(category)) return false;
  auto& file = offsets[static_cast<size_t>(category)];
  uint32_t offset;
  uint8_t header[4];
  if (!file.seek(size_t(index) * 4) || !get32(file, offset) || offset < 5 || !records.seek(offset) ||
      records.read(header, 4) != 4) {
    fail("record seek");
    return false;
  }
  const size_t length = size_t(header[0]) | size_t(header[1]) << 8;
  if (!length || length >= PATH_CAPACITY || header[2] > 1 || !(header[3] & (1 << static_cast<uint8_t>(category))) ||
      records.read(out.path, length) != static_cast<int>(length)) {
    fail("record read");
    return false;
  }
  out.path[length] = 0;
  out.manga = header[2];
  return true;
}
}  // namespace library
