#include <HalStorage.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string_view>

#include "BookmarkStore.h"
#include "util/BookMutationJournal.h"
#include "util/BookMutationOwners.h"
#include "util/BookMutationStorage.h"
using namespace bookmutation;
bool BookmarkStore::folderMutationPath(const char* book, bool legacy, char* out, size_t capacity) {
  uint64_t key = legacy ? std::hash<std::string_view>{}(book) : bookPathHash(book);
  int n = snprintf(out, capacity, "/.crosspoint/bookmarks/manga_%llu.bin", static_cast<unsigned long long>(key));
  return n > 0 && size_t(n) < capacity;
}
bool BookmarkStore::stageForFolderMove(const char* source, const char* oldBook, const char* newBook,
                                       const char* destination, uint8_t* scratch, size_t capacity) {
  if (capacity < 256 || strlen(oldBook) > 1023 || strlen(newBook) > 1023) return false;
  FsFile in = Storage.open(source), out = Storage.open(destination, O_WRONLY | O_CREAT | O_EXCL);
  if (!in || !out) {
    in.close();
    out.close();
    return false;
  }
  uint8_t header[3]{};
  bool ok = in.read(header, 1) == 1;
  uint8_t version = header[0];
  size_t countBytes = version == 2 ? 1 : 2;
  ok = ok && version >= 2 && version <= 5 && in.read(header + 1, countBytes) == int(countBytes);
  uint16_t count = version == 2 ? header[1] : u16(header + 1);
  ok = ok && count <= 1024 && out.write(header, 1 + countBytes) == 1 + countBytes;
  for (unsigned field = 0; ok && field < 3; ++field) {
    uint8_t lengthBytes[4];
    ok = in.read(lengthBytes, 4) == 4;
    if (!ok) break;
    uint32_t length = u32(lengthBytes);
    if (length > 65535 || (field == 2 && length != strlen(oldBook)) || length > in.fileSize64() - in.position()) {
      ok = false;
      break;
    }
    uint8_t replacementLength[4];
    put32(replacementLength, field == 2 ? strlen(newBook) : length);
    ok = out.write(replacementLength, 4) == 4;
    uint32_t offset = 0;
    while (ok && offset < length) {
      size_t n = std::min<size_t>(length - offset, 256);
      ok = in.read(scratch, n) == int(n);
      if (ok && field == 2) ok = !memcmp(scratch, oldBook + offset, n);
      if (ok && field != 2) ok = out.write(scratch, n) == n;
      offset += n;
      serviceMutation();
    }
    if (ok && field == 2) ok = out.write(newBook, strlen(newBook)) == strlen(newBook);
  }
  const size_t recordSize =
      2 + 4 + 4 + BOOKMARK_CHAPTER_TITLE_MAX + (version >= 4 ? 2 : 0) + (version >= 5 ? BOOKMARK_SNIPPET_MAX : 0);
  const uint64_t tail = uint64_t(count) * recordSize;
  ok = ok && in.position() <= in.fileSize64() && in.fileSize64() - in.position() == tail;
  uint64_t left = tail;
  while (ok && left) {
    size_t n = std::min<uint64_t>(left, 256);
    ok = in.read(scratch, n) == int(n) && out.write(scratch, n) == n;
    left -= n;
    serviceMutation();
  }
  const bool synced = out.sync(), closedIn = in.close(), closedOut = out.close();
  return ok && synced && closedIn && closedOut;
}
