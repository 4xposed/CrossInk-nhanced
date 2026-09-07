#include "BookFolderMutation.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string_view>

#include "BookDeletionSnapshot.h"
#include "BookMutationJournal.h"
#include "BookMutationJson.h"
#include "BookMutationOwners.h"
#include "BookMutationStorage.h"
#include "BookmarkStore.h"

namespace BookFolderMutation {
namespace {
using namespace bookmutation;
constexpr char Directory[] = "/.crosspoint/manga-mutation";
constexpr char StagingMarker[] = "/.crosspoint/manga-mutation/staging.bin";
constexpr char FinalizePath[] = "/.crosspoint/manga-mutation.finalize";
constexpr char JournalPath[] = "/.crosspoint/manga-mutation/journal.bin";
constexpr const char* SharedPaths[] = {"/.crosspoint/recent.json", "/.crosspoint/state.json"};
std::atomic<bool> frozen{false}, pending{false};
struct FileRecord {
  uint16_t book = 0;
  uint8_t family = 0;
  Fingerprint source, output;
};
struct SharedRecord {
  bool present = false;
  Fingerprint source, output;
};
class Transaction {
 public:
  // ~17KiB snapshot and3840-byte reusable workspace are fallible transient heap
  // allocations: neither fits a2-4KiB task stack or merits permanent C3 DRAM.
  std::unique_ptr<BookDeletionSnapshot> snapshot = makeUniqueNoThrow<BookDeletionSnapshot>();
  std::unique_ptr<JsonScratch> work = makeUniqueNoThrow<JsonScratch>();
  Header header;
  Replay replay;
  FsFile journal;
  uint64_t filesAt = 0, sharedAt = 0, appendAt = 0;
  bool collision = false, owned = false, sealed = false, finalizing = false;
  uint64_t selected = 0;
  bool ready() {
    if (snapshot && work) return true;
    LOG_ERR("MUTATE", "OOM allocating bounded transaction workspace");
    return false;
  }
  ~Transaction() { journal.close(); }
  void stagePath(unsigned index, char* out, size_t n) { snprintf(out, n, "%s/file-%04u.stage", Directory, index); }
  void sharedPath(unsigned index, bool original, char* out, size_t n) {
    snprintf(out, n, "%s/%s.%s", Directory, index ? "state" : "recent", original ? "original" : "stage");
  }
  Presence presence(const char* path) { return probe(path, work->replacement, work->input); }
  bool write(const void* p, size_t n) { return journal.write(p, n) == n; }
  bool read(void* p, size_t n) { return journal.read(p, n) == int(n); }
  bool record(uint8_t type, uint16_t index, uint32_t bytes) {
    uint8_t p[8]{};
    p[0] = type;
    put16(p + 2, index);
    put32(p + 4, bytes);
    return write(p, sizeof(p));
  }
  bool expect(uint8_t type, uint16_t index, uint32_t& length) {
    uint8_t p[8];
    if (!read(p, 8) || p[0] != type || p[1] || u16(p + 2) != index) return false;
    length = u32(p + 4);
    return length <= 2086 && journal.position() + length <= HeaderBytes + header.payloadBytes;
  }
  bool pathRead(char* out, size_t n) {
    if (n > 1023 || !read(out, n)) return false;
    out[n] = 0;
    return strlen(out) == n;
  }
  bool derived(size_t book, char* out, size_t capacity) {
    const char* old = snapshot->root(snapshot->rootIndex(book));
    const char* path = snapshot->path(book);
    if (!within(path, old)) return false;
    int n = snprintf(out, capacity, "%s%s", snapshot->destination(), path + strlen(old));
    return n > 0 && size_t(n) < capacity && normalizedPath(out);
  }
  bool append(Phase phase, uint16_t index = UINT16_MAX) {
    uint8_t p[PhaseBytes];
    encodePhase(header, replay.sequence + 1, phase, index, p);
    Replay next = replay;
    if (!acceptAppend(header, p, sizeof(p), next) || !journal.seek64(appendAt) || !write(p, sizeof(p)) ||
        !journal.sync())
      return false;
    uint8_t check[PhaseBytes];
    if (!journal.seek64(appendAt) || !read(check, sizeof(check)) || memcmp(check, p, sizeof(p))) return false;
    appendAt += sizeof(p);
    replay = next;
    return true;
  }
  bool outcome(const Outcome& value) {
    uint8_t p[OutcomeBytes];
    encodeOutcome(header, replay.sequence + 1, value, p);
    Replay next = replay;
    if (!acceptAppend(header, p, sizeof(p), next) || !journal.seek64(appendAt) || !write(p, sizeof(p)) ||
        !journal.sync())
      return false;
    uint8_t check[OutcomeBytes];
    if (!journal.seek64(appendAt) || !read(check, sizeof(check)) || memcmp(check, p, sizeof(p))) return false;
    appendAt += sizeof(p);
    replay = next;
    return true;
  }
  bool writeRoots() {
    for (size_t i = 0; i < snapshot->rootCount(); ++i) {
      const char* root = snapshot->root(i);
      size_t a = strlen(root), b = header.operation == Operation::Move ? strlen(snapshot->destination()) : 0;
      uint8_t p[4];
      put16(p, a);
      put16(p + 2, b);
      if (!record(1, i, 4 + a + b) || !write(p, 4) || !write(root, a) || (b && !write(snapshot->destination(), b)))
        return false;
    }
    for (size_t i = 0; i < snapshot->count(); ++i) {
      size_t n = strlen(snapshot->path(i));
      uint8_t p[6]{};
      put16(p, snapshot->rootIndex(i));
      p[2] = uint8_t(snapshot->kind(i));
      put16(p + 4, n);
      if (!record(2, i, 6 + n) || !write(p, 6) || !write(snapshot->path(i), n)) return false;
    }
    filesAt = journal.position();
    return true;
  }
  bool writeFile(const FileRecord& f, const char* src, const char* dst) {
    uint8_t p[32]{};
    size_t a = strlen(src), b = strlen(dst);
    put16(p, f.book);
    p[2] = f.family;
    put16(p + 4, a);
    put16(p + 6, b);
    put64(p + 8, f.source.bytes);
    put64(p + 16, f.output.bytes);
    put32(p + 24, f.source.crc);
    put32(p + 28, f.output.crc);
    return record(3, header.files, 32 + a + b) && write(p, 32) && write(src, a) && write(dst, b);
  }
  bool readFile(unsigned index, FileRecord& f) {
    uint32_t length;
    uint8_t p[32];
    if (!expect(3, index, length) || length < 32 || !read(p, 32) || p[3]) return false;
    f.book = u16(p);
    f.family = p[2];
    f.source = {u64(p + 8), u32(p + 24)};
    f.output = {u64(p + 16), u32(p + 28)};
    size_t a = u16(p + 4), b = u16(p + 6);
    if (f.book >= header.books || f.family < 1 || f.family > 4 || length != 32 + a + b || !pathRead(work->book, a) ||
        !pathRead(work->value, b))
      return false;
    if (!derived(f.book, work->replacement, sizeof(work->replacement))) return false;
    const auto oldKey = bookPathHash(snapshot->path(f.book)), newKey = bookPathHash(work->replacement);
    if (f.family == 2 || f.family == 3) {
      if (!bookmarkPath(work->replacement, f.family == 3, work->replacement, sizeof(work->replacement)) ||
          strcmp(work->value, work->replacement))
        return false;
      return bookmarkPath(snapshot->path(f.book), f.family == 3, work->replacement, sizeof(work->replacement)) &&
             !strcmp(work->book, work->replacement);
    }
    if (!(f.source == f.output)) return false;
    if (f.family == 1) {
      for (const char* suffix : {".bin", ".bin.bak", ".bin.tmp"}) {
        snprintf(work->replacement, sizeof(work->replacement), "/.crosspoint/manga-state/%lu%s",
                 static_cast<unsigned long>(oldKey), suffix);
        if (strcmp(work->book, work->replacement)) continue;
        snprintf(work->replacement, sizeof(work->replacement), "/.crosspoint/manga-state/%lu%s",
                 static_cast<unsigned long>(newKey), suffix);
        return !strcmp(work->value, work->replacement);
      }
      return false;
    }
    const char* name = strrchr(work->book, '/');
    if (!name || !durableCacheName(++name)) return false;
    snprintf(work->replacement, sizeof(work->replacement), "/.crosspoint/manga_%lu/%s",
             static_cast<unsigned long>(oldKey), name);
    if (strcmp(work->book, work->replacement)) return false;
    snprintf(work->replacement, sizeof(work->replacement), "/.crosspoint/manga_%lu/%s",
             static_cast<unsigned long>(newKey), name);
    return !strcmp(work->value, work->replacement);
  }
  bool readShared(unsigned index, SharedRecord& f) {
    uint32_t length;
    uint8_t p[28];
    if (!expect(4, index, length) || length != 28 || !read(p, 28) || p[0] != index || p[1] > 1 ||
        u16(p + 2) != (header.operation == Operation::Delete ? 1 : 0))
      return false;
    f.present = p[1];
    f.source = {u64(p + 4), u32(p + 20)};
    f.output = {u64(p + 12), u32(p + 24)};
    return (f.present || (f.source.bytes == 0 && f.source.crc == 0)) &&
           (header.operation == Operation::Move || (f.output.bytes == 0 && f.output.crc == 0));
  }
  bool stageBookmark(const char* source, const char* oldBook, const char* newBook, const char* stage,
                     Fingerprint& original, Fingerprint& output);
  bool stageOne(size_t book, uint8_t family, const char* source, const char* destination) {
    if (header.files >= 1024) return false;
    auto p = presence(source);
    if (p == Presence::Missing) return true;
    if (p != Presence::File) return false;
    if (presence(destination) != Presence::Missing) {
      collision = true;
      return false;
    }
    char stage[96];
    stagePath(header.files, stage, sizeof(stage));
    FileRecord f;
    f.book = book;
    f.family = family;
    bool ok;
    if (family == 2 || family == 3) {
      if (!derived(book, work->replacement, sizeof(work->replacement))) return false;
      ok = stageBookmark(source, snapshot->path(book), work->replacement, stage, f.source, f.output);
    } else {
      ok = copyFile(source, stage, f.source, work->input);
      f.output = f.source;
    }
    if (!ok || !writeFile(f, source, destination)) return false;
    ++header.files;
    return true;
  }
  bool destinationKeys(bool checkPhysical = true) {
    for (size_t i = 0; i < snapshot->count(); ++i) {
      if (!derived(i, work->value, sizeof(work->value))) return false;
      const auto src = bookPathHash(snapshot->path(i)), dst = bookPathHash(work->value);
      if (src == dst) {
        collision = true;
        return false;
      }
      // Keys are hash-only: reject every source/destination alias in the snapshot.
      const auto sourceLegacy = std::hash<std::string_view>{}(snapshot->path(i)),
                 destLegacy = std::hash<std::string_view>{}(work->value);
      if (sourceLegacy == destLegacy || src == destLegacy || dst == sourceLegacy) {
        collision = true;
        return false;
      }
      for (size_t j = 0; j < snapshot->count(); ++j) {
        if (i == j) continue;
        if (!derived(j, work->book, sizeof(work->book))) return false;
        const auto otherSrc = bookPathHash(snapshot->path(j)), otherDst = bookPathHash(work->book);
        const auto otherLegacySrc = std::hash<std::string_view>{}(snapshot->path(j)),
                   otherLegacyDst = std::hash<std::string_view>{}(work->book);
        if (src == otherSrc || src == otherDst || dst == otherSrc || dst == otherDst ||
            sourceLegacy == otherLegacySrc || sourceLegacy == otherLegacyDst || destLegacy == otherLegacySrc ||
            destLegacy == otherLegacyDst || src == otherLegacySrc || src == otherLegacyDst || dst == otherLegacySrc ||
            dst == otherLegacyDst || sourceLegacy == otherSrc || sourceLegacy == otherDst || destLegacy == otherSrc ||
            destLegacy == otherDst) {
          collision = true;
          return false;
        }
        serviceMutation();
      }
      if (!checkPhysical) continue;
      if (!mangaCachePath(work->value, work->book, sizeof(work->book)) || presence(work->book) != Presence::Missing) {
        collision = true;
        return false;
      }
      for (const char* suffix : {".bin", ".bin.bak", ".bin.tmp"}) {
        snprintf(work->book, sizeof(work->book), "/.crosspoint/manga-state/%lu%s", static_cast<unsigned long>(dst),
                 suffix);
        if (presence(work->book) != Presence::Missing) {
          collision = true;
          return false;
        }
      }
      for (bool legacy : {false, true}) {
        if (!bookmarkPath(work->value, legacy, work->book, sizeof(work->book)) ||
            presence(work->book) != Presence::Missing) {
          collision = true;
          return false;
        }
      }
    }
    return true;
  }
  bool stageFiles() {
    for (size_t i = 0; i < snapshot->count(); ++i) {
      const unsigned begin = header.files;
      const auto src = bookPathHash(snapshot->path(i));
      if (!derived(i, work->value, sizeof(work->value))) return false;
      const auto dst = bookPathHash(work->value);
      for (const char* suffix : {".bin", ".bin.bak", ".bin.tmp"}) {
        snprintf(work->book, sizeof(work->book), "/.crosspoint/manga-state/%lu%s", static_cast<unsigned long>(src),
                 suffix);
        snprintf(work->value, sizeof(work->value), "/.crosspoint/manga-state/%lu%s", static_cast<unsigned long>(dst),
                 suffix);
        if (!stageOne(i, 1, work->book, work->value)) return false;
      }
      for (bool legacy : {false, true}) {
        if (!derived(i, work->replacement, sizeof(work->replacement)) ||
            !bookmarkPath(snapshot->path(i), legacy, work->book, sizeof(work->book)) ||
            !bookmarkPath(work->replacement, legacy, work->value, sizeof(work->value)) ||
            !stageOne(i, legacy ? 3 : 2, work->book, work->value))
          return false;
      }
      if (!mangaCachePath(snapshot->path(i), work->book, sizeof(work->book))) return false;
      auto p = presence(work->book);
      if (p == Presence::Missing) continue;
      if (p != Presence::Directory) return false;
      FsFile dir = Storage.open(work->book);
      if (!dir) return false;
      bool ok = true;
      for (FsFile child = dir.openNextFileChecked(); child; child = dir.openNextFileChecked()) {
        char* name = reinterpret_cast<char*>(work->probeName);
        size_t n = child.getName(name, 256);
        bool directory = child.isDirectory();
        bool closed = child.close();
        if (!closed || !n || n >= 255) {
          ok = false;
          break;
        }
        if (!durableCacheName(name)) continue;
        if (directory || header.files - begin >= 16) {
          ok = false;
          break;
        }
        snprintf(work->book, sizeof(work->book), "/.crosspoint/manga_%lu/%s", static_cast<unsigned long>(src), name);
        snprintf(work->value, sizeof(work->value), "/.crosspoint/manga_%lu/%s", static_cast<unsigned long>(dst), name);
        if (!stageOne(i, 4, work->book, work->value)) {
          ok = false;
          break;
        }
        serviceMutation();
      }
      ok = !dir.enumerationFailed() && ok;
      ok = dir.close() && ok;
      if (!ok) return false;
    }
    return true;
  }
  static PathEdit pathEdit(void* context, PathField field, const char* book, const char* value, char* out,
                           size_t capacity) {
    return static_cast<Transaction*>(context)->edit(field, book, value, out, capacity);
  }
  PathEdit edit(PathField field, const char* book, const char* value, char* out, size_t capacity) {
    for (size_t i = 0; i < snapshot->count(); ++i) {
      const char* path = snapshot->path(i);
      if (field == PathField::Cover) {
        if (strcmp(book, path) || header.operation == Operation::Delete) continue;
        char cache[64];
        mangaCachePath(path, cache, sizeof(cache));
        if (!within(value, cache)) continue;
        // Destination path can use the output buffer temporarily; cache keys are short.
        if (!derived(i, out, capacity)) return PathEdit::Error;
        const auto hash = bookPathHash(out);
        int n = snprintf(out, capacity, "/.crosspoint/manga_%lu%s", static_cast<unsigned long>(hash),
                         value + strlen(cache));
        return n > 0 && size_t(n) < capacity ? PathEdit::Replace : PathEdit::Error;
      }
      if (!strcmp(value, path)) {
        if (header.operation == Operation::Delete)
          return selected & (uint64_t(1) << i) ? PathEdit::Remove : PathEdit::Keep;
        return derived(i, out, capacity) ? PathEdit::Replace : PathEdit::Error;
      }
    }
    if (header.operation == Operation::Move && (field == PathField::Book || field == PathField::Resume)) {
      for (size_t i = 0; i < snapshot->count(); ++i) {
        if (!derived(i, out, capacity)) return PathEdit::Error;
        if (!strcmp(value, out)) {
          collision = true;
          return PathEdit::Error;
        }
      }
    }
    if (header.operation == Operation::Delete && (field == PathField::Favorite || field == PathField::Preferred) &&
        *value) {
      for (size_t i = 0; i < snapshot->rootCount(); ++i)
        if (within(value, snapshot->root(i))) {
          auto p = probe(value, work->book, work->probeName);
          if (p == Presence::Missing) return PathEdit::Remove;
          if (p == Presence::Error) return PathEdit::Error;
        }
    }
    return PathEdit::Keep;
  }
  bool sharedOutput(unsigned index, Fingerprint& result) {
    char original[96], stage[96];
    sharedPath(index, true, original, sizeof(original));
    sharedPath(index, false, stage, sizeof(stage));
    // Checked enumeration opens each entry, including journal.bin. Release its
    // sole reader while probing a sibling stage, then restore the write cursor.
    const uint64_t cursor = journal.position();
    if (!journal.close()) return false;
    const bool removed = ownedRemove(stage, work->replacement, work->input);
    journal = Storage.open(finalizing ? FinalizePath : JournalPath, O_RDWR);
    if (!journal || !journal.seek64(cursor) || !removed) return false;
    FsFile in = Storage.open(original), out = Storage.open(stage, O_WRONLY | O_CREAT | O_EXCL);
    if (!in || !out) {
      in.close();
      out.close();
      return false;
    }
    bool ok = rewriteSharedJson(in, out, index ? JsonKind::State : JsonKind::Recent, pathEdit, this, *work);
    bool sync = out.sync();
    bool ci = in.close(), co = out.close();
    return ok && sync && ci && co && validateSharedOwner(!index, stage) && fingerprint(stage, result, work->input);
  }
  bool sharedOriginal(unsigned index, SharedRecord& record) {
    char original[96];
    sharedPath(index, true, original, sizeof(original));
    auto p = presence(SharedPaths[index]);
    if (p == Presence::Error || p == Presence::Directory) return false;
    record.present = p == Presence::File;
    if (record.present) {
      if (!copyFile(SharedPaths[index], original, record.source, work->input)) return false;
    } else {
      // A missing JSON store must not hide an unmigrated binary owner snapshot.
      if (presence(index ? "/.crosspoint/state.bin" : "/.crosspoint/recent.bin") != Presence::Missing) return false;
      FsFile file = Storage.open(original, O_WRONLY | O_CREAT | O_EXCL);
      const char* empty = index ? "{}" : "{\"books\":[]}";
      if (!file) return false;
      bool ok = file.write(empty, strlen(empty)) == strlen(empty) && file.sync();
      ok = file.close() && ok;
      if (!ok) return false;
    }
    // A checked streaming owner parse validates the original before PREPARED.
    if (!validateSharedOwner(!index, original)) return false;
    Fingerprint output;
    if (!sharedOutput(index, output)) return false;
    if (header.operation == Operation::Move) record.output = output;
    return true;
  }
  bool stageShared() {
    sharedAt = journal.position();
    for (unsigned i = 0; i < 2; ++i) {
      SharedRecord f;
      if (!sharedOriginal(i, f)) return false;
      uint8_t p[28]{};
      p[0] = i;
      p[1] = f.present;
      put16(p + 2, header.operation == Operation::Delete ? 1 : 0);
      put64(p + 4, f.source.bytes);
      put64(p + 12, f.output.bytes);
      put32(p + 20, f.source.crc);
      put32(p + 24, f.output.crc);
      if (!record(4, i, sizeof(p)) || !write(p, sizeof(p))) return false;
    }
    return true;
  }
  bool stagingMarker(bool createMarker) {
    uint8_t bytes[24]{};
    memcpy(bytes, "CMI1", 4);
    put64(bytes + 4, header.id);
    put32(bytes + 20, crc32(bytes, 20));
    if (createMarker) {
      FsFile file = Storage.open(StagingMarker, O_WRONLY | O_CREAT | O_EXCL);
      if (!file) return false;
      bool ok = file.write(bytes, 24) == 24 && file.sync();
      ok = file.close() && ok;
      if (!ok) return false;
    }
    FsFile file = Storage.open(StagingMarker);
    if (!file) return false;
    uint8_t actual[24];
    bool ok = file.fileSize64() == 24 && file.read(actual, 24) == 24 && !memcmp(actual, "CMI1", 4) && u64(actual + 4) &&
              u32(actual + 20) == crc32(actual, 20);
    for (unsigned i = 12; ok && i < 20; ++i) ok = actual[i] == 0;
    return file.close() && ok;
  }
  bool create() {
    if (!Storage.ensureDirectoryExists("/.crosspoint") || presence(Directory) != Presence::Missing ||
        !Storage.mkdir(Directory, false))
      return false;
    pending.store(true);
    owned = true;
    header.id = mutationId();
    if (!header.id) header.id = 1;
    if (!stagingMarker(true)) return false;
    journal = Storage.open(JournalPath, O_RDWR | O_CREAT | O_EXCL);
    if (!journal) return false;
    uint8_t blank[HeaderBytes]{};
    header.roots = snapshot->rootCount();
    header.books = snapshot->count();
    if (!write(blank, sizeof(blank)) || !writeRoots()) return false;
    if (header.operation == Operation::Move && !stageFiles()) return false;
    if (!stageShared()) return false;
    appendAt = journal.position();
    if (appendAt < HeaderBytes || appendAt - HeaderBytes > MaxPayloadBytes) return false;
    header.payloadBytes = appendAt - HeaderBytes;
    if (!journal.seek64(HeaderBytes)) return false;
    uint32_t left = header.payloadBytes;
    while (left) {
      size_t n = std::min<uint32_t>(left, 256);
      if (!read(work->input, n)) return false;
      header.payloadCrc = crc32(work->input, n, header.payloadCrc);
      left -= n;
      serviceMutation();
    }
    uint8_t h[HeaderBytes];
    encodeHeader(header, h);
    if (!journal.seek64(0) || !write(h, sizeof(h)) || !journal.sync()) return false;
    uint8_t check[HeaderBytes];
    if (!journal.seek64(0) || !read(check, sizeof(check)) || memcmp(check, h, sizeof(h))) return false;
    sealed = true;
    if (!Storage.remove(StagingMarker)) return false;
    return append(Phase::Prepared);
  }
  bool load();
  bool verifyOriginals();
  bool verifyFinalOutputs();
  bool token(const char* root, bool createToken = false, bool removeToken = false);
  bool publishFiles();
  bool publishReferences();
  bool removeSources();
  bool cleanup();
  Result recoverMove();
  Result recoverDelete(bool partial = true);
};
bool Transaction::stageBookmark(const char* source, const char* oldBook, const char* newBook, const char* stage,
                                Fingerprint& original, Fingerprint& output) {
  return fingerprint(source, original, work->input) &&
         BookmarkStore::stageForFolderMove(source, oldBook, newBook, stage, work->input, 256) &&
         fingerprint(stage, output, work->input);
}
bool Transaction::load() {
  journal = Storage.open(finalizing ? FinalizePath : JournalPath, O_RDWR);
  if (!journal) return false;
  owned = true;
  sealed = true;
  uint8_t h[HeaderBytes];
  if (!read(h, sizeof(h)) || !decodeHeader(h, header) || journal.fileSize64() < HeaderBytes + header.payloadBytes)
    return false;
  uint32_t left = header.payloadBytes, crc = 0;
  while (left) {
    size_t n = std::min<uint32_t>(left, 256);
    if (!read(work->input, n)) return false;
    crc = crc32(work->input, n, crc);
    left -= n;
    serviceMutation();
  }
  if (crc != header.payloadCrc || !journal.seek64(HeaderBytes)) return false;
  snapshot->reset();
  for (unsigned i = 0; i < header.roots; ++i) {
    uint32_t length;
    uint8_t p[4];
    if (!expect(1, i, length) || length < 4 || !read(p, 4)) return false;
    size_t a = u16(p), b = u16(p + 2);
    if (length != 4 + a + b || !a || !pathRead(work->book, a) || !normalizedPath(work->book) ||
        !snapshot->restoreRoot(work->book) || !pathRead(work->value, b))
      return false;
    if (header.operation == Operation::Move) {
      if (!b || !normalizedPath(work->value) || !snapshot->setDestination(work->value)) return false;
    } else if (b)
      return false;
  }
  for (unsigned i = 0; i < header.books; ++i) {
    uint32_t length;
    uint8_t p[6];
    if (!expect(2, i, length) || length < 6 || !read(p, 6) || p[3]) return false;
    size_t n = u16(p + 4);
    if (length != 6 + n || p[2] > 1 || u16(p) >= header.roots || !pathRead(work->book, n) ||
        !normalizedPath(work->book) || !within(work->book, snapshot->root(u16(p))) ||
        (header.operation == Operation::Move && p[2] != 1) ||
        !snapshot->restoreEntry(work->book, BookDeletionSnapshot::Kind(p[2]), u16(p)))
      return false;
  }
  if (header.operation == Operation::Move && !destinationKeys(false)) return false;
  filesAt = journal.position();
  unsigned perBook[64]{};  // 256 bytes, bounded counters; no durable-file vector.
  for (unsigned i = 0; i < header.files; ++i) {
    FileRecord f;
    if (!readFile(i, f) || ++perBook[f.book] > 16) return false;
    const uint64_t next = journal.position();
    // Equal source/destination bytes in a second record are never an ownership proof.
    const uint32_t sourceKey = crc32(work->book, strlen(work->book)), destKey = crc32(work->value, strlen(work->value));
    if (!journal.seek64(filesAt)) return false;
    for (unsigned j = 0; j < i; ++j) {
      FileRecord previous;
      if (!readFile(j, previous) || crc32(work->book, strlen(work->book)) == sourceKey ||
          crc32(work->value, strlen(work->value)) == destKey)
        return false;
      serviceMutation();
    }
    if (!journal.seek64(next)) return false;
  }
  sharedAt = journal.position();
  for (unsigned i = 0; i < 2; ++i) {
    SharedRecord f;
    if (!readShared(i, f)) return false;
  }
  appendAt = journal.position();
  if (appendAt != HeaderBytes + header.payloadBytes) return false;
  const uint64_t size = journal.fileSize64();
  while (appendAt < size) {
    uint64_t remaining = size - appendAt;
    if (remaining < 4) break;
    uint8_t p[OutcomeBytes];
    if (!read(p, 4)) return false;
    size_t n = !memcmp(p, "CMP1", 4) ? PhaseBytes : !memcmp(p, "CMO1", 4) ? OutcomeBytes : 0;
    if (!n) return false;
    if (remaining < n) break;
    if (!read(p + 4, n - 4)) return false;
    if (u32(p + n - 4) != crc32(p, n - 4)) {
      if (remaining != n) return false;
      break;
    }
    if (!acceptAppend(header, p, n, replay)) return false;
    appendAt += n;
  }
  // Only the CRC/length-classified final torn append is discarded, never an
  // interior or semantically invalid record. Truncate before the next append.
  if (appendAt < size && (!journal.truncate(appendAt) || !journal.sync() || journal.fileSize64() != appendAt))
    return false;
  return true;
}
bool Transaction::verifyFinalOutputs() {
  if (replay.aborted) return true;
  if (!replay.done || !journal.seek64(filesAt)) return false;
  for (unsigned i = 0; i < header.files; ++i) {
    FileRecord record;
    Fingerprint actual;
    if (!readFile(i, record) || !fingerprint(work->value, actual, work->input) || !(actual == record.output))
      return false;
    serviceMutation();
  }
  if (!journal.seek64(sharedAt)) return false;
  for (unsigned i = 0; i < 2; ++i) {
    SharedRecord record;
    Fingerprint actual;
    if (!readShared(i, record)) return false;
    const auto expected = header.operation == Operation::Move ? record.output
                          : i ? Fingerprint{replay.outcome.stateBytes, replay.outcome.stateCrc}
                              : Fingerprint{replay.outcome.recentBytes, replay.outcome.recentCrc};
    if (!fingerprint(SharedPaths[i], actual, work->input) || !(actual == expected)) return false;
  }
  return true;
}
bool Transaction::verifyOriginals() {
  if (!journal.seek64(sharedAt)) return false;
  for (unsigned i = 0; i < 2; ++i) {
    SharedRecord record;
    if (!readShared(i, record)) return false;
    char path[96];
    sharedPath(i, true, path, sizeof(path));
    Fingerprint actual;
    if (!fingerprint(path, actual, work->input)) return false;
    if (record.present) {
      if (!(record.source == actual)) return false;
    } else {
      const char* value = i ? "{}" : "{\"books\":[]}";
      if (actual.bytes != strlen(value) || actual.crc != crc32(value, strlen(value))) return false;
    }
    // Validation of this frozen original was required before PREPARED; unchanged
    // length/CRC binds recovery to the same complete checked owner snapshot.
  }
  return true;
}
bool Transaction::token(const char* root, bool createToken, bool removeToken) {
  const int n = snprintf(work->book, sizeof(work->book), "%s/.crossink-move-token", root);
  if (n < 0 || size_t(n) >= sizeof(work->book)) return false;
  uint8_t expected[24]{}, headerBytes[HeaderBytes];
  encodeHeader(header, headerBytes);
  memcpy(expected, "CMT1", 4);
  put64(expected + 4, header.id);
  put32(expected + 12, u32(headerBytes + 44));
  put32(expected + 20, crc32(expected, 20));
  if (createToken) {
    if (presence(work->book) != Presence::Missing) return false;
    FsFile out = Storage.open(work->book, O_WRONLY | O_CREAT | O_EXCL);
    if (!out) return false;
    bool ok = out.write(expected, sizeof(expected)) == sizeof(expected) && out.sync();
    ok = out.close() && ok;
    if (!ok) return false;
  }
  FsFile in = Storage.open(work->book);
  if (!in) return false;
  uint8_t actual[24];
  bool ok = in.fileSize64() == 24 && in.read(actual, sizeof(actual)) == sizeof(actual) &&
            !memcmp(actual, expected, sizeof(actual));
  ok = in.close() && ok;
  if (!ok) return false;
  return !removeToken || Storage.remove(work->book);
}
bool Transaction::publishFiles() {
  if (!journal.seek64(filesAt)) return false;
  for (unsigned i = 0; i < header.files; ++i) {
    FileRecord f;
    if (!readFile(i, f)) return false;
    uint64_t next = journal.position();
    char stage[96];
    stagePath(i, stage, sizeof(stage));
    Fingerprint actual;
    auto p = presence(work->value);
    if (p == Presence::Missing) {
      if (i < replay.published || !fingerprint(stage, actual, work->input) || !(actual == f.output) ||
          !makeParent(work->value, work->replacement, work->input) || presence(work->value) != Presence::Missing ||
          !Storage.rename(stage, work->value))
        return false;
    } else if (p != Presence::File)
      return false;
    if (!fingerprint(work->value, actual, work->input) || !(actual == f.output)) return false;
    if (i >= replay.published && !append(Phase::FilePublished, i)) return false;
    if (!journal.seek64(next)) return false;
    serviceMutation();
  }
  return true;
}
bool Transaction::publishReferences() {
  if (!journal.seek64(sharedAt)) return false;
  for (unsigned i = 0; i < 2; ++i) {
    SharedRecord record;
    if (!readShared(i, record)) return false;
    uint64_t next = journal.position();
    Fingerprint expected = record.output;
    if (header.operation == Operation::Delete)
      expected = i ? Fingerprint{replay.outcome.stateBytes, replay.outcome.stateCrc}
                   : Fingerprint{replay.outcome.recentBytes, replay.outcome.recentCrc};
    char stage[96];
    sharedPath(i, false, stage, sizeof(stage));
    Fingerprint actual;
    auto p = presence(SharedPaths[i]);
    bool final = p == Presence::File && fingerprint(SharedPaths[i], actual, work->input) && actual == expected;
    if (!final) {
      if (p == Presence::File) {
        if (!record.present || !(actual == record.source) || !Storage.remove(SharedPaths[i])) return false;
      } else if (p != Presence::Missing)
        return false;
      if (!fingerprint(stage, actual, work->input) || !(actual == expected) ||
          presence(SharedPaths[i]) != Presence::Missing || !Storage.rename(stage, SharedPaths[i]) ||
          !fingerprint(SharedPaths[i], actual, work->input) || !(actual == expected))
        return false;
    }
    if (!(i ? replay.state : replay.recent) && !append(i ? Phase::StatePublished : Phase::RecentPublished))
      return false;
    if (!journal.seek64(next)) return false;
  }
  if (!replay.references && !append(Phase::ReferencesDone)) return false;
  return true;
}
bool Transaction::removeSources() {
  if (!journal.seek64(filesAt)) return false;
  for (unsigned i = 0; i < header.files; ++i) {
    FileRecord f;
    if (!readFile(i, f)) return false;
    uint64_t next = journal.position();
    auto p = presence(work->book);
    if (p == Presence::File) {
      Fingerprint actual;
      if (!fingerprint(work->book, actual, work->input) || !(actual == f.source) || !Storage.remove(work->book))
        return false;
    } else if (p != Presence::Missing)
      return false;
    if (i >= replay.removed && !append(Phase::SourceRemoved, i)) return false;
    if (!journal.seek64(next)) return false;
    serviceMutation();
  }
  return true;
}
bool Transaction::cleanup() {
  if (!owned) return true;
  if (sealed && !replay.done) {
    if (!journal.truncate(appendAt) || !append(Phase::Aborted)) return false;
  }
  if (!journal.close()) return false;
  // Stages are in an exclusively owned namespace. Unknown names are never removed.
  auto directoryPresence = presence(Directory);
  if (directoryPresence == Presence::Directory) {
    FsFile dir = Storage.open(Directory);
    if (!dir) return false;
    bool ok = true;
    for (FsFile child = dir.openNextFileChecked(); child; child = dir.openNextFileChecked()) {
      char name[96];
      size_t n = child.getName(name, sizeof(name));
      bool directory = child.isDirectory();
      bool closed = child.close();
      if (!closed || directory || !n || n >= sizeof(name) - 1) {
        ok = false;
        break;
      }
      bool allowed = !strcmp(name, "recent.original") || !strcmp(name, "recent.stage") ||
                     !strcmp(name, "state.original") || !strcmp(name, "state.stage");
      if (n == 15 && !strncmp(name, "file-", 5) && !strcmp(name + 9, ".stage")) {
        allowed = true;
        unsigned index = 0;
        for (unsigned i = 5; i < 9; ++i) {
          if (name[i] < '0' || name[i] > '9') {
            allowed = false;
            break;
          }
          index = index * 10 + name[i] - '0';
        }
        if (index >= 1024) allowed = false;
      }
      if (!strcmp(name, "journal.bin") || !strcmp(name, "staging.bin")) continue;
      if (!allowed) {
        ok = false;
        break;
      }
      snprintf(work->book, sizeof(work->book), "%s/%s", Directory, name);
      if (!Storage.remove(work->book)) {
        ok = false;
        break;
      }
      serviceMutation();
    }
    ok = !dir.enumerationFailed() && ok;
    ok = dir.close() && ok;
    if (!ok) return false;
  } else if (directoryPresence != Presence::Missing)
    return false;
  if (!finalizing) {
    if (presence(FinalizePath) != Presence::Missing) return false;
    if (sealed) {
      if (!ownedRemove(StagingMarker, work->replacement, work->input) || !Storage.rename(JournalPath, FinalizePath))
        return false;
    } else {
      if (!stagingMarker(false) || !ownedRemove(JournalPath, work->replacement, work->input) ||
          !Storage.rename(StagingMarker, FinalizePath))
        return false;
    }
    finalizing = true;
  }
  directoryPresence = presence(Directory);
  if (directoryPresence == Presence::Directory && !Storage.rmdir(Directory)) return false;
  if (directoryPresence != Presence::Directory && directoryPresence != Presence::Missing) return false;
  owned = false;
  return true;
}

Result Transaction::recoverMove() {
  if (replay.done)
    return (verifyFinalOutputs() && cleanup()) ? (replay.aborted ? Result::MutationFailed : Result::Complete)
                                               : Result::RecoveryPending;
  if (!replay.prepared) return cleanup() ? Result::MutationFailed : Result::RecoveryPending;
  if (!verifyOriginals()) return Result::RecoveryPending;
  const auto old = presence(snapshot->root(0)), destination = presence(snapshot->destination());
  if (!replay.moved && old == Presence::Directory && destination == Presence::Missing && token(snapshot->root(0))) {
    if (!token(snapshot->root(0), false, true) || !cleanup()) return Result::RecoveryPending;
    return Result::MutationFailed;
  }
  if (old != Presence::Missing || destination != Presence::Directory) return Result::RecoveryPending;
  if (!replay.tokenRemoved && !token(snapshot->destination())) {
    // A power failure can fall between owned token removal and its phase append;
    // completed reference/source phases and all final fingerprints prove cleanup.
    if (!replay.references || replay.removed != header.files) return Result::RecoveryPending;
  } else if (!replay.moved && !append(Phase::Moved))
    return Result::RecoveryPending;
  if (!publishFiles() || !publishReferences() || !removeSources()) return Result::RecoveryPending;
  if (!replay.tokenRemoved) {
    if (token(snapshot->destination())) {
      if (!token(snapshot->destination(), false, true)) return Result::RecoveryPending;
    }
    if (!append(Phase::TokenRemoved)) return Result::RecoveryPending;
  }
  if (!replay.done && !append(Phase::Done)) return Result::RecoveryPending;
  return cleanup() ? Result::Complete : Result::RecoveryPending;
}
bool removeMangaMetadata(Transaction& tx, const char* book) {
  const auto hash = bookPathHash(book);
  for (const char* suffix : {".bin", ".bin.bak", ".bin.tmp"}) {
    snprintf(tx.work->book, sizeof(tx.work->book), "/.crosspoint/manga-state/%lu%s", static_cast<unsigned long>(hash),
             suffix);
    if (!ownedRemove(tx.work->book, tx.work->replacement, tx.work->input)) return false;
  }
  for (bool legacy : {false, true})
    if (!bookmarkPath(book, legacy, tx.work->book, sizeof(tx.work->book)) ||
        !ownedRemove(tx.work->book, tx.work->replacement, tx.work->input))
      return false;
  mangaCachePath(book, tx.work->book, sizeof(tx.work->book));
  auto p = tx.presence(tx.work->book);
  if (p == Presence::Missing) return true;
  if (p != Presence::Directory) return false;
  // Iterative postorder traversal reuses the existing path/name scratch. Reopen
  // the parent after removing a child directory; no recursive stack or heap.
  const size_t rootLength = strlen(tx.work->book);
  for (;;) {
    FsFile dir = Storage.open(tx.work->book);
    if (!dir || !dir.isDirectory()) {
      dir.close();
      return false;
    }
    const size_t parentLength = strlen(tx.work->book);
    bool descend = false, ok = true;
    for (FsFile child = dir.openNextFileChecked(); child; child = dir.openNextFileChecked()) {
      char* name = reinterpret_cast<char*>(tx.work->probeName);
      const size_t n = child.getName(name, 256);
      const bool directory = child.isDirectory();
      const bool closed = child.close();
      if (!closed || !n || n >= 255 || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..") ||
          parentLength + 1 + n >= sizeof(tx.work->book)) {
        ok = false;
        break;
      }
      tx.work->book[parentLength] = '/';
      memcpy(tx.work->book + parentLength + 1, name, n + 1);
      if (directory) {
        descend = true;
        break;
      }
      ok = Storage.remove(tx.work->book);
      tx.work->book[parentLength] = 0;
      serviceMutation();
      if (!ok) break;
    }
    ok = !dir.enumerationFailed() && !dir.allocationFailed() && ok;
    ok = dir.close() && ok;
    if (!ok) return false;
    serviceMutation();
    if (descend) continue;
    if (!Storage.rmdir(tx.work->book)) return false;
    if (parentLength == rootLength) return tx.presence(tx.work->book) == Presence::Missing;
    char* slash = strrchr(tx.work->book, '/');
    if (!slash || size_t(slash - tx.work->book) < rootLength) return false;
    *slash = 0;
  }
}
Result Transaction::recoverDelete(bool partial) {
  if (replay.done)
    return (verifyFinalOutputs() && cleanup())
               ? ((replay.aborted || replay.outcome.partial) ? Result::MutationFailed : Result::Complete)
               : Result::RecoveryPending;
  if (!replay.prepared || !replay.deleteStarted) return cleanup() ? Result::MutationFailed : Result::RecoveryPending;
  if (!verifyOriginals()) return Result::RecoveryPending;
  uint64_t absent = 0;
  for (size_t i = 0; i < snapshot->count(); ++i) {
    auto p = presence(snapshot->path(i));
    if (p == Presence::Error) return Result::RecoveryPending;
    if (p == Presence::Missing) absent |= uint64_t(1) << i;
  }
  if (replay.hasOutcome) {
    if ((absent & replay.outcome.absent) != replay.outcome.absent) return Result::RecoveryPending;
    selected = replay.outcome.absent;
  } else {
    selected = absent;
    Outcome out;
    out.absent = absent;
    out.partial = partial;
    Fingerprint r, s;
    if (!sharedOutput(0, r) || !sharedOutput(1, s)) return Result::RecoveryPending;
    out.recentBytes = r.bytes;
    out.recentCrc = r.crc;
    out.stateBytes = s.bytes;
    out.stateCrc = s.crc;
    if (!outcome(out)) return Result::RecoveryPending;
  }
  for (size_t i = 0; i < snapshot->count(); ++i) {
    const auto bit = uint64_t(1) << i;
    if (!(selected & bit) || replay.cleaned & bit) continue;
    if (presence(snapshot->path(i)) != Presence::Missing) return Result::RecoveryPending;
    bool ok = snapshot->kind(i) == BookDeletionSnapshot::Kind::Manga
                  ? removeMangaMetadata(*this, snapshot->path(i))
                  : removeFileMetadataChecked(snapshot->path(i), work->book, work->replacement, work->input);
    if (!ok || !append(Phase::DeleteMetadataDone, i)) return Result::RecoveryPending;
  }
  if (!publishReferences() || (!replay.done && !append(Phase::Done))) return Result::RecoveryPending;
  bool failed = replay.outcome.partial;
  return cleanup() ? (failed ? Result::MutationFailed : Result::Complete) : Result::RecoveryPending;
}
Result recoverInternal() {
  Transaction tx;
  if (!tx.ready()) return Result::StorageError;
  auto final = tx.presence(FinalizePath);
  if (final == Presence::File) {
    FsFile file = Storage.open(FinalizePath);
    uint8_t marker[24];
    bool isMarker = file && file.fileSize64() == 24 && file.read(marker, 24) == 24 && !memcmp(marker, "CMI1", 4) &&
                    u64(marker + 4) && u32(marker + 20) == crc32(marker, 20);
    for (unsigned i = 12; isMarker && i < 20; ++i) isMarker = marker[i] == 0;
    bool closed = file.close();
    if (!closed) return Result::RecoveryPending;
    tx.owned = tx.finalizing = true;
    pending.store(true);
    if (isMarker) return tx.cleanup() ? Result::MutationFailed : Result::RecoveryPending;
    if (!tx.load() || !tx.replay.done || !tx.verifyFinalOutputs()) return Result::RecoveryPending;
    return tx.cleanup() ? Result::Complete : Result::RecoveryPending;
  } else if (final != Presence::Missing) {
    pending.store(true);
    return Result::RecoveryPending;
  }
  auto p = tx.presence(Directory);
  if (p == Presence::Missing) {
    pending.store(false);
    return Result::Complete;
  }
  pending.store(true);
  if (p != Presence::Directory) return Result::RecoveryPending;
  if (tx.stagingMarker(false)) {
    tx.owned = true;
    return tx.cleanup() ? Result::MutationFailed : Result::RecoveryPending;
  }
  if (!tx.load()) return Result::RecoveryPending;
  return tx.header.operation == Operation::Move ? tx.recoverMove() : tx.recoverDelete();
}

struct Gate {
  bool owned = false;
  Gate() {
    bool expected = false;
    owned = frozen.compare_exchange_strong(expected, true);
  }
  ~Gate() {
    if (owned) frozen.store(false);
  }
};

}  // namespace
bool hasPending() { return pending.load(); }
bool storesFrozen() { return frozen.load() || pending.load(); }
int httpStatus(Result r, bool overwrite) {
  if (r == Result::Busy || r == Result::RecoveryPending) return 503;
  if (r == Result::Collision) return overwrite ? 409 : 412;
  if (r == Result::InvalidPath) return 400;
  return 500;
}
const char* error(Result r) {
  switch (r) {
    case Result::Busy:
      return "busy";
    case Result::InvalidPath:
      return "invalid_path";
    case Result::SnapshotLimit:
      return "snapshot_limit";
    case Result::Collision:
      return "target_exists";
    case Result::RecoveryPending:
      return "metadata_recovery_pending";
    case Result::MutationFailed:
      return "mutation_failed";
    case Result::NotManga:
      return "not_manga";
    default:
      return "storage_error";
  }
}
}  // namespace BookFolderMutation

namespace BookFolderMutation {
Result finishOwners(Result result) {
  if (!pending.load() || result == Result::RecoveryPending) return result;
  // No Transaction is alive here. Its21KiB buffers are free before cold owner loads.
  FsFile final = Storage.open(FinalizePath);
  if (!final) return Result::RecoveryPending;
  uint8_t bytes[24];
  bool check = final.fileSize64() >= 24 && final.read(bytes, 24) == 24;
  bool reload = true;
  if (check && !memcmp(bytes, "CMI1", 4)) {
    check = final.fileSize64() == 24 && u64(bytes + 4) && u32(bytes + 20) == crc32(bytes, 20);
    for (unsigned i = 12; check && i < 20; ++i) check = bytes[i] == 0;
    reload = false;
  } else {
    check = check && !memcmp(bytes, "CMJ1", 4) && final.seek64(final.fileSize64() - 24) &&
            final.read(bytes, 24) == 24 && !memcmp(bytes, "CMP1", 4) && u32(bytes + 20) == crc32(bytes, 20) &&
            (bytes[8] == uint8_t(Phase::Done) || bytes[8] == uint8_t(Phase::Aborted));
    reload = bytes[8] != uint8_t(Phase::Aborted);
  }
  check = final.close() && check;
  if (!check || (reload && !reloadSharedOwners()) || !Storage.remove(FinalizePath)) return Result::RecoveryPending;
  pending.store(false);
  return result;
}
Result recoverPending() {
  Gate gate;
  if (!gate.owned) return Result::Busy;
  const auto result = finishOwners(recoverInternal());
  if (result == Result::StorageError) pending.store(true);
  return result;
}
Result retryPendingMutation() {
  if (!hasPending()) return Result::Complete;
  Gate gate;
  if (!gate.owned || !quiesceOwners()) return Result::Busy;
  return finishOwners(recoverInternal());
}
Result moveImpl(const char* oldRoot, const char* newRoot) {
  auto recovered = finishOwners(recoverInternal());
  if (recovered == Result::RecoveryPending || recovered == Result::StorageError) return recovered;
  Transaction tx;
  if (!tx.ready()) return Result::StorageError;
  if (!normalizedPath(oldRoot) || !normalizedPath(newRoot) || !strcasecmp(oldRoot, newRoot) ||
      within(newRoot, oldRoot) || within(oldRoot, newRoot))
    return Result::InvalidPath;
  if (tx.presence(oldRoot) != Presence::Directory) return Result::StorageError;
  auto dst = tx.presence(newRoot);
  if (dst == Presence::Error) return Result::StorageError;
  if (!tx.snapshot->collect(oldRoot, SnapshotMode::MangaMove) || !tx.snapshot->setDestination(newRoot))
    return Result::SnapshotLimit;
  if (!tx.snapshot->count()) return Result::NotManga;
  if (dst != Presence::Missing) return Result::Collision;
  if (!tx.destinationKeys()) return tx.collision ? Result::Collision : Result::StorageError;
  tx.header.operation = Operation::Move;
  if (!tx.create()) {
    bool cleaned = tx.cleanup();
    return cleaned ? (tx.collision ? Result::Collision : Result::StorageError) : Result::RecoveryPending;
  }
  if (tx.presence(oldRoot) != Presence::Directory || tx.presence(newRoot) != Presence::Missing ||
      !tx.token(oldRoot, true))
    return Result::RecoveryPending;
  const bool renamed = Storage.rename(oldRoot, newRoot);
  (void)renamed;
  return tx.recoverMove();
}
Result move(const char* oldRoot, const char* newRoot) {
  Gate gate;
  if (!gate.owned || !quiesceOwners()) return Result::Busy;
  return finishOwners(moveImpl(oldRoot, newRoot));
}
Result remove(const char* root, uint8_t maxDepth) { return removeMany(&root, 1, maxDepth); }
Result removeManyImpl(const char* const* roots, size_t count, uint8_t maxDepth) {
  auto recovered = finishOwners(recoverInternal());
  if (recovered == Result::RecoveryPending || recovered == Result::StorageError) return recovered;
  if (!roots || !count || count > 64) return Result::SnapshotLimit;
  Transaction tx;
  if (!tx.ready()) return Result::StorageError;
  tx.header.operation = Operation::Delete;
  for (size_t i = 0; i < count; ++i)
    if (!normalizedPath(roots[i])) return Result::InvalidPath;
  for (size_t i = 0; i < count; ++i) {
    bool skip = false;
    for (size_t j = 0; j < count; ++j)
      if (i != j && roots[j] && within(roots[i], roots[j]) && (strcmp(roots[i], roots[j]) || j < i)) {
        skip = true;
        break;
      }
    if (skip) continue;
    auto p = tx.presence(roots[i]);
    if (p == Presence::Missing || p == Presence::Error) return Result::StorageError;
    if (!tx.snapshot->append(roots[i], SnapshotMode::DeleteMetadata, maxDepth)) return Result::SnapshotLimit;
  }
  if (!tx.create()) {
    return tx.cleanup() ? Result::StorageError : Result::RecoveryPending;
  }
  if (!tx.append(Phase::DeleteStarted)) return Result::RecoveryPending;
  bool success = true;
  for (size_t i = 0; i < tx.snapshot->rootCount(); ++i) {
    const char* path = tx.snapshot->root(i);
    auto p = tx.presence(path);
    bool removed = p == Presence::Directory ? Storage.removeDir(path)
                   : p == Presence::File    ? Storage.remove(path)
                                            : false;
    success = removed && success;
    serviceMutation();
  }
  if (!tx.append(success ? Phase::ContentSucceeded : Phase::ContentFailed)) return Result::RecoveryPending;
  return tx.recoverDelete(!success);
}
Result removeMany(const char* const* roots, size_t count, uint8_t maxDepth) {
  Gate gate;
  if (!gate.owned || !quiesceOwners()) return Result::Busy;
  return finishOwners(removeManyImpl(roots, count, maxDepth));
}

}  // namespace BookFolderMutation
