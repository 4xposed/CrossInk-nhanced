#include "BookMutationStorage.h"

#include <Logging.h>
#include <Memory.h>
#include <uzlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string_view>

#include "BookFolderMutation.h"
#include "BookMutationJournal.h"
#include "BookMutationOwners.h"
namespace bookmutation {
bool normalizedPath(const char* p) {
  if (!p || p[0] != '/' || !p[1]) return false;
  const size_t n = strlen(p);
  if (n > 1023 || p[n - 1] == '/') return false;
  for (const char* s = p + 1; *s;) {
    const char* end = strchr(s, '/');
    size_t length = end ? size_t(end - s) : strlen(s);
    if (!length || length > 255 || (length == 1 && s[0] == '.') || (length == 2 && s[0] == '.' && s[1] == '.'))
      return false;
    for (size_t i = 0; i < length; ++i)
      if (s[i] == '\\' || static_cast<unsigned char>(s[i]) < 32) return false;
    if ((length == 11 && !strncasecmp(s, ".crosspoint", 11)) ||
        (length == 20 && !strncasecmp(s, ".crossink-move-token", 20)) ||
        (length == 25 && !strncasecmp(s, "System Volume Information", 25)) ||
        (length == 7 && !strncasecmp(s, "XTCache", 7)))
      return false;
    s = end ? end + 1 : s + length;
  }
  return true;
}
bool within(const char* p, const char* root) {
  size_t n = strlen(root);
  return !strncmp(p, root, n) && (p[n] == 0 || p[n] == '/');
}
Presence probe(const char* path, char* scratch, uint8_t* name) {
  if (!Storage.ready() || !path || path[0] != '/' || strlen(path) > 1023) return Presence::Error;
  strcpy(scratch, "/");
  const char* component = path + 1;
  FsFile dir = Storage.open("/");
  if (!dir || !dir.isDirectory()) {
    dir.close();
    return Presence::Error;
  }
  if (!*component) return dir.close() ? Presence::Directory : Presence::Error;
  while (*component) {
    const char* slash = strchr(component, '/');
    size_t n = slash ? size_t(slash - component) : strlen(component);
    if (!n || n > 255) {
      dir.close();
      return Presence::Error;
    }
    bool found = false, isDir = false;
    for (FsFile child = dir.openNextFileChecked(); child; child = dir.openNextFileChecked()) {
      size_t length = child.getName(reinterpret_cast<char*>(name), 256);
      bool closed = child.close();
      serviceMutation();
      if (!closed || !length || length >= 255) {
        dir.close();
        return Presence::Error;
      }
      if (length == n && !strncasecmp(reinterpret_cast<char*>(name), component, n)) {
        if (strncmp(reinterpret_cast<char*>(name), component, n)) {
          dir.close();
          return Presence::Error;
        }
        found = true;
        break;
      }
    }
    bool good = !dir.enumerationFailed() && !dir.allocationFailed();
    good = dir.close() && good;
    if (!good) return Presence::Error;
    if (!found) return Presence::Missing;
    size_t used = strlen(scratch);
    if (used > 1) scratch[used++] = '/';
    memcpy(scratch + used, component, n);
    scratch[used + n] = 0;
    FsFile next = Storage.open(scratch);
    if (!next) return Presence::Error;
    isDir = next.isDirectory();
    if (!slash) return next.close() ? (isDir ? Presence::Directory : Presence::File) : Presence::Error;
    if (!isDir) {
      next.close();
      return Presence::Error;
    }
    dir = std::move(next);
    component = slash + 1;
  }
  dir.close();
  return Presence::Error;
}
bool fingerprint(const char* path, Fingerprint& result, uint8_t* buffer) {
  FsFile file = Storage.open(path);
  if (!file || file.isDirectory()) {
    file.close();
    return false;
  }
  Fingerprint value;
  value.bytes = file.fileSize64();
  uint64_t remaining = value.bytes;
  bool ok = true;
  while (remaining && ok) {
    size_t n = std::min<uint64_t>(remaining, 256);
    ok = file.read(buffer, n) == int(n);
    if (ok) {
      value.crc = crc32(buffer, n, value.crc);
      remaining -= n;
    }
    serviceMutation();
  }
  ok = file.close() && ok;
  if (ok) result = value;
  return ok;
}
bool copyFile(const char* source, const char* destination, Fingerprint& result, uint8_t* buffer) {
  FsFile in = Storage.open(source);
  if (!in || in.isDirectory()) {
    in.close();
    return false;
  }
  FsFile out = Storage.open(destination, O_WRONLY | O_CREAT | O_EXCL);
  if (!out) {
    in.close();
    return false;
  }
  Fingerprint value;
  value.bytes = in.fileSize64();
  uint64_t remaining = value.bytes;
  bool ok = true;
  while (remaining && ok) {
    size_t n = std::min<uint64_t>(remaining, 256);
    ok = in.read(buffer, n) == int(n) && out.write(buffer, n) == n;
    if (ok) {
      value.crc = crc32(buffer, n, value.crc);
      remaining -= n;
    }
    serviceMutation();
  }
  const bool synced = out.sync();
  const bool closedOut = out.close();
  const bool closedIn = in.close();
  ok = ok && synced && closedOut && closedIn;
  Fingerprint checked;
  if (ok) ok = fingerprint(destination, checked, buffer) && checked == value;
  if (ok) result = value;
  return ok;
}
bool ownedRemove(const char* path, char* scratch, uint8_t* name) {
  auto present = probe(path, scratch, name);
  if (present == Presence::Missing) return true;
  if (present != Presence::File) return false;
  return Storage.remove(path);
}
bool makeParent(const char* path, char* scratch, uint8_t* name) {
  (void)name;
  const size_t length = strlen(path);
  if (length > 1023) return false;
  memcpy(scratch, path, length + 1);
  char* slash = strrchr(scratch, '/');
  if (!slash) return false;
  *slash = 0;
  // Generated metadata parents only; mkdir's checked result retains the journal on failure.
  if (!*scratch) return true;
  return Storage.ensureDirectoryExists(scratch);
}
bool durableCacheName(const char* name) {
  if (!strcasecmp(name, "dictionary.bin") || !strcasecmp(name, "dictionary_history.txt")) return true;
  const char* p = name;
  if (strncasecmp(p, "stats", 5)) return false;
  p += 5;
  if (*p == '_') {
    if (p[1] != 'v' && p[1] != 'V') return false;
    p += 2;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') ++p;
  }
  if (strncasecmp(p, ".bin", 4)) return false;
  p += 4;
  return !*p || !strcasecmp(p, ".tmp") || !strcasecmp(p, ".bak") || !strcasecmp(p, ".recovery") ||
         !strcasecmp(p, ".invalid");
}
uint32_t bookPathHash(const char* path) { return uzlib_crc32(path, static_cast<unsigned>(strlen(path)), 0); }
bool mangaCachePath(const char* book, char* out, size_t capacity) {
  int n = snprintf(out, capacity, "/.crosspoint/manga_%lu", static_cast<unsigned long>(bookPathHash(book)));
  return n > 0 && size_t(n) < capacity;
}
// Manga history/stats stay in place while only disposable cache children are
// removed. This also preserves the sole valid .bak/.recovery after a failed save.
bool clearMangaDisposableCache(const std::string& cache) {
  if (BookFolderMutation::storesFrozen()) return false;
  auto scratch = makeUniqueNoThrow<char[]>(1536);  // Fallible paths+name workspace, not task stack.
  if (!scratch) {
    LOG_ERR("BookCache", "OOM clearing manga cache");
    return false;
  }
  const auto presence =
      bookmutation::probe(cache.c_str(), scratch.get(), reinterpret_cast<uint8_t*>(scratch.get() + 1024));
  if (presence == bookmutation::Presence::Missing) return true;
  if (presence != bookmutation::Presence::Directory) return false;
  FsFile directory = Storage.open(cache.c_str());
  if (!directory) return false;
  bool ok = true;
  for (FsFile child = directory.openNextFileChecked(); child; child = directory.openNextFileChecked()) {
    char* name = scratch.get() + 1024;
    const size_t n = child.getName(name, 256);
    const bool folder = child.isDirectory();
    if (!child.close() || !n || n >= 255) {
      ok = false;
      break;
    }
    if (bookmutation::durableCacheName(name)) continue;
    const int length = snprintf(scratch.get(), 1024, "%s/%s", cache.c_str(), name);
    if (length < 0 || length >= 1024 || !(folder ? Storage.removeDir(scratch.get()) : Storage.remove(scratch.get()))) {
      ok = false;
      break;
    }
    serviceMutation();
  }
  ok = !directory.enumerationFailed() && ok;
  return directory.close() && ok;
}

bool removeBookFileMetadata(const char* book, char* path, char* scratch, uint8_t* names) {
  const char* dot = strrchr(book, '.');
  if (!dot) return true;
  const char* type = nullptr;
  if (!strcasecmp(dot, ".epub"))
    type = "epub";
  else if (!strcasecmp(dot, ".xtc") || !strcasecmp(dot, ".xtch"))
    type = "xtc";
  else if (!strcasecmp(dot, ".txt") || !strcasecmp(dot, ".md"))
    type = "txt";
  if (!strcasecmp(dot, ".cdeck")) {
    uint64_t hash = 14695981039346656037ULL;
    for (const auto* p = reinterpret_cast<const unsigned char*>(book); *p; ++p) {
      hash ^= *p;
      hash *= 1099511628211ULL;
    }
    // AnkiDeck::load derives this identity solely from the path. Do not attempt
    // to reload deleted content to discover its metadata directory.
    snprintf(path, 1024, "/.crosspoint/anki_%016llx", static_cast<unsigned long long>(hash));
    const auto present = probe(path, scratch, names);
    return present == Presence::Missing || (present == Presence::Directory && Storage.removeDir(path) &&
                                            probe(path, scratch, names) == Presence::Missing);
  }
  if (!type) return true;
  const uint64_t crc = bookPathHash(book), legacy = std::hash<std::string_view>{}(book);
  for (uint64_t key : {crc, legacy}) {
    snprintf(path, 1024, "/.crosspoint/bookmarks/%s_%llu.bin", type, static_cast<unsigned long long>(key));
    if (!ownedRemove(path, scratch, names)) return false;
  }
  if (!strcmp(type, "epub")) {
    snprintf(path, 1024, "/.crosspoint/clippings/epub_%llu.bin", static_cast<unsigned long long>(crc));
    if (!ownedRemove(path, scratch, names)) return false;
    // Same FNV-1a identity as Epub::cachePathForFilePath, without constructing a
    // parser or migrating a legacy cache after its content has already vanished.
    uint64_t hash = 14695981039346656037ULL;
    for (const auto* p = reinterpret_cast<const unsigned char*>(book); *p; ++p) {
      hash ^= *p;
      hash *= 1099511628211ULL;
    }
    for (uint64_t key : {hash, legacy}) {
      snprintf(path, 1024, "/.crosspoint/epub_%llu", static_cast<unsigned long long>(key));
      auto present = probe(path, scratch, names);
      if (present == Presence::Missing) continue;
      if (present != Presence::Directory || !Storage.removeDir(path) ||
          probe(path, scratch, names) != Presence::Missing)
        return false;
    }
  }
  return true;
}
}  // namespace bookmutation
