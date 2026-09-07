#include "BookDeletionSnapshot.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cctype>
#include <cstring>

namespace {
bool endsWithIgnoreCase(const char* value, const char* suffix) {
  const size_t valueLength = strlen(value);
  const size_t suffixLength = strlen(suffix);
  if (valueLength < suffixLength) return false;
  value += valueLength - suffixLength;
  for (size_t i = 0; i < suffixLength; ++i) {
    if (tolower(static_cast<unsigned char>(value[i])) != tolower(static_cast<unsigned char>(suffix[i]))) return false;
  }
  return true;
}

bool hasFileMetadata(const char* name) {
  return endsWithIgnoreCase(name, ".epub") || endsWithIgnoreCase(name, ".cdeck") || endsWithIgnoreCase(name, ".xtc") ||
         endsWithIgnoreCase(name, ".xtch") || endsWithIgnoreCase(name, ".txt") || endsWithIgnoreCase(name, ".md");
}
}  // namespace

bool BookDeletionSnapshot::appendPath(const char* parent, const char* name, size_t nameLength, uint16_t& offset) {
  const size_t parentLength = strlen(parent);
  const bool slash = parentLength == 0 || parent[parentLength - 1] != '/';
  const size_t length = parentLength + (slash ? 1 : 0) + nameLength;
  if (length > 1023 || length + 1 > ARENA_BYTES - arenaUsed_) return false;
  offset = static_cast<uint16_t>(arenaUsed_);
  char* out = arena_.data() + arenaUsed_;
  memcpy(out, parent, parentLength);
  size_t write = parentLength;
  if (slash) out[write++] = '/';
  memcpy(out + write, name, nameLength);
  out[length] = '\0';
  arenaUsed_ += length + 1;
  return true;
}

void BookDeletionSnapshot::reset() {
  arenaUsed_ = directoryCount_ = entryCount_ = rootCount_ = 0;
  destinationOffset_ = 0;
}
bool BookDeletionSnapshot::storePath(const char* path, uint16_t& offset) {
  const size_t size = strlen(path) + 1;
  if (size > 1024 || size > ARENA_BYTES - arenaUsed_) return false;
  offset = static_cast<uint16_t>(arenaUsed_);
  memcpy(arena_.data() + arenaUsed_, path, size);
  arenaUsed_ += size;
  return true;
}
bool BookDeletionSnapshot::restoreRoot(const char* path) {
  if (rootCount_ >= MAX_DIRECTORIES || !storePath(path, roots_[rootCount_])) return false;
  ++rootCount_;
  return true;
}
bool BookDeletionSnapshot::restoreEntry(const char* path, Kind kind, uint8_t root) {
  if (entryCount_ >= MAX_ENTRIES || root >= rootCount_) return false;
  uint16_t offset;
  if (!storePath(path, offset)) return false;
  entries_[entryCount_++] = {offset, kind, root};
  return true;
}
bool BookDeletionSnapshot::setDestination(const char* path) { return storePath(path, destinationOffset_); }
bool BookDeletionSnapshot::collect(const std::string& root, SnapshotMode mode, uint8_t maxDepth) {
  return collect(root.c_str(), mode, maxDepth);
}
bool BookDeletionSnapshot::collect(const char* root, SnapshotMode mode, uint8_t maxDepth) {
  reset();
  return append(root, mode, maxDepth);
}

bool BookDeletionSnapshot::append(const char* root, SnapshotMode mode, uint8_t maxDepth) {
  if (!root || !*root || !restoreRoot(root)) return false;
  const uint8_t rootIndex = static_cast<uint8_t>(rootCount_ - 1);
  FsFile probe = Storage.open(root);
  if (!probe) return false;
  const bool isDirectory = probe.isDirectory();
  if (!probe.close()) return false;
  if (!isDirectory) {
    return mode == SnapshotMode::MangaMove || !hasFileMetadata(root) || restoreEntry(root, Kind::File, rootIndex);
  }
  if (directoryCount_ >= MAX_DIRECTORIES) return false;
  const size_t first = directoryCount_;
  directories_[directoryCount_] = roots_[rootIndex];
  depths_[directoryCount_++] = 0;
  char name[256];
  for (size_t index = first; index < directoryCount_; ++index) {
    const uint16_t directoryOffset = directories_[index];
    const char* directoryPath = arena_.data() + directoryOffset;
    FsFile directory = Storage.open(directoryPath);
    if (!directory || !directory.isDirectory()) {
      if (directory) directory.close();
      LOG_ERR("BookDelete", "Failed to open directory: %s", directoryPath);
      return false;
    }
    bool manga = false;
    for (FsFile file = directory.openNextFileChecked(); file; file = directory.openNextFileChecked()) {
      const size_t length = file.getName(name, sizeof(name));
      const bool isDirectory = file.isDirectory();
      const bool closeOk = file.close();
      if (!closeOk || length == 0 || length >= sizeof(name) - 1 || (isDirectory && depths_[index] >= maxDepth)) {
        directory.close();
        entryCount_ = 0;
        return false;
      }
      if (!isDirectory && strcasecmp(name, "panels.idx") == 0) manga = true;
      if (!isDirectory && (mode == SnapshotMode::MangaMove || !hasFileMetadata(name))) continue;
      if (isDirectory && directoryCount_ >= MAX_DIRECTORIES) {
        directory.close();
        entryCount_ = 0;
        return false;
      }
      if (!isDirectory && entryCount_ >= MAX_ENTRIES) {
        directory.close();
        entryCount_ = 0;
        return false;
      }
      uint16_t childOffset = 0;
      if (!appendPath(directoryPath, name, length, childOffset)) {
        directory.close();
        entryCount_ = 0;
        return false;
      }
      if (isDirectory) {
        directories_[directoryCount_] = childOffset;
        depths_[directoryCount_++] = depths_[index] + 1;
      } else
        entries_[entryCount_++] = {childOffset, Kind::File, rootIndex};
    }
    if (directory.enumerationFailed() || directory.allocationFailed()) {
      directory.close();
      entryCount_ = 0;
      return false;
    }
    if (!directory.close()) {
      entryCount_ = 0;
      return false;
    }
    if (manga) {
      if (entryCount_ >= MAX_ENTRIES) {
        entryCount_ = 0;
        return false;
      }
      entries_[entryCount_++] = {directoryOffset, Kind::Manga, rootIndex};
    }
  }
  return true;
}
