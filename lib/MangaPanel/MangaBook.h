#pragma once

#include <CooperativeCancellation.h>

#include <memory>

#include "MangaFormat.h"

namespace manga {
// Foreground-owned, not thread safe. No file handles survive an operation.
// Page views (including nested text) expire on ANY loadPage/open/close, even failure.
// Metadata views survive navigation until open/close. TOC labels expire on ANY
// readTocEntry/open/close. Copy text before retaining it across those calls.
enum class PathResult { Found, Missing, Error };
enum class OpenMode { Reader, Metadata, Cover, Index };

class MangaBook {
 public:
  MangaBook() = default;
  MangaBook(const MangaBook&) = delete;
  MangaBook& operator=(const MangaBook&) = delete;
  static bool isMangaFolder(const char* folder);
  // Paths are copied to caller storage, cleared on Missing/Error. Missing means absent
  // or a directory; HAL cannot distinguish a failed existence probe from absence.
  PathResult pageImagePath(uint32_t index, char* out, size_t size, CooperativeCancellation cancellation = {});
  PathResult panelImagePath(uint32_t page, uint16_t panel, char* out, size_t size);
  // Library callers avoid the reader page/TOC buffers. Cover mode also skips metadata.
  bool open(const char* folder, OpenMode mode = OpenMode::Reader, CooperativeCancellation cancellation = {});
  void close();
  uint32_t pageCount() const { return pageCount_; }
  bool readPageInfo(uint32_t index, format::IndexRecord& out);
  bool loadPage(uint32_t index, format::PageView& out);
  std::string_view title() const;
  std::string_view author() const { return meta_.author; }
  std::string_view language() const { return meta_.language; }
  uint32_t tocCount() const { return tocCount_; }
  bool readTocEntry(uint32_t index, format::TocEntryView& out);

 private:
  enum class StorageFormat : uint8_t { None, Legacy, Mokuro };
  bool setFolder(const char* folder);
  const char* path(const char* name);
  bool loadIndex(bool allocatePage, CooperativeCancellation cancellation);
  bool discoverImages(CooperativeCancellation cancellation);
  bool classifyImages(CooperativeCancellation cancellation);
  bool imagesClassified_ = false;
  PathResult probeFile(const char* name);
  PathResult copyPath(const char* name, char* out, size_t size);
  PathResult legacyPage(uint32_t index, CooperativeCancellation cancellation);
  struct ImageScan {
    char last[256]{}, best[256]{}, entry[256]{};
    uint64_t lastOrdinal = 0;
    uint32_t index = 0;
    bool hasLast = false;
  };
  std::unique_ptr<ImageScan> imageScan_;
  const char* canonicalExtension_ = nullptr;
  bool panelsDirectory_ = false;
  void loadMeta();
  void loadToc();
  std::unique_ptr<char[]> folder_, path_;
  size_t folderLength_ = 0;
  uint32_t pageCount_ = 0, pageCapacity_ = 0, tocCount_ = 0;
  uint16_t tocCapacity_ = 0;
  uint64_t dataSize_ = 0, tocNextOffset_ = 8;
  uint32_t tocNextIndex_ = 0;
  uint32_t mokuroVersion_ = 0;
  StorageFormat storageFormat_ = StorageFormat::None;
  std::unique_ptr<uint8_t[]> page_, metaBytes_;
  std::unique_ptr<char[]> tocLabel_;
  format::MetaView meta_;
};
}  // namespace manga
