#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

enum class SnapshotMode : uint8_t { DeleteMetadata, MangaMove };

class BookDeletionSnapshot {
 public:
  enum class Kind : uint8_t { File, Manga };
  struct Entry {
    uint16_t offset = 0;
    Kind kind = Kind::File;
    uint8_t root = 0;
  };

  static constexpr size_t ARENA_BYTES = 16 * 1024;
  static constexpr size_t MAX_DIRECTORIES = 64;
  static constexpr size_t MAX_ENTRIES = 64;

  bool collect(const std::string& root, SnapshotMode mode = SnapshotMode::DeleteMetadata, uint8_t maxDepth = 255);
  bool collect(const char* root, SnapshotMode mode = SnapshotMode::DeleteMetadata, uint8_t maxDepth = 255);
  bool append(const char* root, SnapshotMode mode = SnapshotMode::DeleteMetadata, uint8_t maxDepth = 255);
  void reset();
  // Checked journal restoration; no filesystem reads or content mutations.
  bool restoreRoot(const char* root);
  bool restoreEntry(const char* path, Kind kind, uint8_t root);
  bool setDestination(const char* path);
  const char* destination() const { return arena_.data() + destinationOffset_; }
  size_t rootCount() const { return rootCount_; }
  const char* root(size_t i) const { return arena_.data() + roots_[i]; }
  uint8_t rootIndex(size_t i) const { return entries_[i].root; }
  size_t count() const { return entryCount_; }
  const char* path(size_t index) const { return arena_.data() + entries_[index].offset; }
  Kind kind(size_t index) const { return entries_[index].kind; }

 private:
  bool appendPath(const char* parent, const char* name, size_t nameLength, uint16_t& offset);
  std::array<char, ARENA_BYTES> arena_{};
  std::array<uint16_t, MAX_DIRECTORIES> directories_{};
  std::array<uint16_t, MAX_DIRECTORIES> roots_{};
  std::array<uint8_t, MAX_DIRECTORIES> depths_{};
  std::array<Entry, MAX_ENTRIES> entries_{};
  size_t arenaUsed_ = 0;
  size_t directoryCount_ = 0;
  size_t entryCount_ = 0;
  size_t rootCount_ = 0;
  uint16_t destinationOffset_ = 0;
  bool storePath(const char* path, uint16_t& offset);
};
