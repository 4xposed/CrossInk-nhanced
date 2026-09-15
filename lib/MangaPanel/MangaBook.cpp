#include "MangaBook.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <NaturalSort.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>

namespace manga {
namespace {
constexpr size_t kMokuroIndexHeaderBytes = 12;
constexpr size_t kMokuroIndexRecordBytes = 20;
constexpr uint32_t kMokuroAdapterHeaderBytes = 14;
constexpr uint32_t kMokuroMaxRecordBytes = format::kMaxPageBytes - kMokuroAdapterHeaderBytes;
constexpr uint16_t kMokuroMaxWidth = 528;
constexpr uint16_t kMokuroMaxHeight = 800;

uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
uint32_t u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
bool finish(FsFile& file, bool success, const char* context) {
  const bool closed = file.close();
  if (!success || !closed) LOG_ERR("MNG", "%s: read/seek/format/close failed", context);
  (void)context;
  return success && closed;
}
bool openFile(const char* path, FsFile& file) {
  file = Storage.open(path);
  if (!file) {
    file.close();
    LOG_ERR("MNG", "Cannot open %s", path);
    return false;
  }
  if (file.isDirectory()) return finish(file, false, path);
  return true;
}
bool read(FsFile& file, void* data, size_t size) {
  return size == 0 || file.read(data, size) == static_cast<int>(size);
}

void putU16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
}

bool decodeMokuroHeader(const uint8_t* bytes, uint64_t indexSize, format::IndexHeader& out) {
  out = {};
  if (memcmp(bytes, "CMI1", 4) != 0 || (u32(bytes + 4) < 1 || u32(bytes + 4) > 3)) return false;
  const uint32_t count = u32(bytes + 8);
  if (count == 0 || count > format::kMaxPages) return false;
  if (indexSize != kMokuroIndexHeaderBytes + uint64_t(count) * kMokuroIndexRecordBytes) return false;
  out.pageCount = count;
  return true;
}

bool decodeMokuroRecord(const uint8_t* bytes, uint64_t dataSize, uint32_t version, format::IndexRecord& out) {
  out = {};
  const format::IndexRecord record{u32(bytes), u32(bytes + 4), u16(bytes + 8), u16(bytes + 10)};
  const bool portraitBounds =
      record.imageWidth <= (version == 1 ? 480 : kMokuroMaxWidth) && record.imageHeight <= kMokuroMaxHeight;
  const bool landscapeBounds =
      version == 3 && record.imageWidth <= kMokuroMaxHeight && record.imageHeight <= kMokuroMaxWidth;
  if (record.dataLength < 4 || record.dataLength > kMokuroMaxRecordBytes || record.imageWidth == 0 ||
      record.imageHeight == 0 || (!portraitBounds && !landscapeBounds) || u16(bytes + 18) != 0 ||
      record.dataOffset > dataSize || record.dataLength > dataSize - record.dataOffset) {
    return false;
  }
  out = record;
  return true;
}

bool validUtf8WithoutNul(const uint8_t* bytes, size_t size) {
  size_t offset = 0;
  while (offset < size) {
    const uint8_t first = bytes[offset++];
    if (first == 0) return false;
    if (first < 0x80) continue;
    size_t continuation = 0;
    uint8_t secondMin = 0x80;
    uint8_t secondMax = 0xbf;
    if (first >= 0xc2 && first <= 0xdf) {
      continuation = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      continuation = 2;
      if (first == 0xe0) secondMin = 0xa0;
      if (first == 0xed) secondMax = 0x9f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      continuation = 3;
      if (first == 0xf0) secondMin = 0x90;
      if (first == 0xf4) secondMax = 0x8f;
    } else {
      return false;
    }
    if (continuation > size - offset || bytes[offset] < secondMin || bytes[offset] > secondMax) return false;
    for (size_t i = 1; i < continuation; ++i) {
      if (bytes[offset + i] < 0x80 || bytes[offset + i] > 0xbf) return false;
    }
    offset += continuation;
  }
  return true;
}

bool adaptMokuroPage(uint8_t* page, uint32_t recordLength, const format::IndexRecord& record, uint32_t& adaptedLength) {
  adaptedLength = 0;
  const uint8_t* const raw = page + kMokuroAdapterHeaderBytes;
  const uint16_t blockCount = u16(raw);
  if (u16(raw + 2) != 0 || blockCount > 255) return false;

  size_t sourceOffset = 4;
  size_t destinationOffset = kMokuroAdapterHeaderBytes;
  for (uint16_t block = 0; block < blockCount; ++block) {
    if (recordLength - sourceOffset < 12) return false;
    const uint8_t* const source = raw + sourceOffset;
    const uint16_t x = u16(source);
    const uint16_t y = u16(source + 2);
    const uint16_t width = u16(source + 4);
    const uint16_t height = u16(source + 6);
    const uint16_t textLength = u16(source + 8);
    const uint16_t flags = u16(source + 10);
    if (width == 0 || height == 0 || x >= record.imageWidth || y >= record.imageHeight ||
        width > record.imageWidth - x || height > record.imageHeight - y || (flags & ~uint16_t{1}) != 0 ||
        textLength > recordLength - sourceOffset - 12 || !validUtf8WithoutNul(source + 12, textLength)) {
      return false;
    }
    memmove(page + destinationOffset, source, 10);
    memmove(page + destinationOffset + 10, source + 12, textLength);
    sourceOffset += 12u + textLength;
    destinationOffset += 10u + textLength;
  }
  if (sourceOffset != recordLength) return false;

  page[0] = 1;
  page[1] = 0;
  putU16(page + 2, 0);
  putU16(page + 4, 0);
  putU16(page + 6, record.imageWidth);
  putU16(page + 8, record.imageHeight);
  page[10] = static_cast<uint8_t>(blockCount);
  page[11] = 0;
  putU16(page + 12, 0);
  adaptedLength = static_cast<uint32_t>(destinationOffset);
  return true;
}
}  // namespace

void MangaBook::close() {
  meta_ = {};
  imageScan_.reset();
  canonicalExtension_ = nullptr;
  imagesClassified_ = false;
  panelsDirectory_ = false;
  pageCount_ = pageCapacity_ = tocCount_ = 0;
  tocCapacity_ = 0;
  tocNextOffset_ = 8;
  tocNextIndex_ = 0;
  storageFormat_ = StorageFormat::None;
  mokuroVersion_ = 0;
  dataSize_ = folderLength_ = 0;
  page_.reset();
  metaBytes_.reset();
  tocLabel_.reset();
  path_.reset();
  folder_.reset();
}
bool MangaBook::setFolder(const char* folder) {
  if (!folder || !*folder) {
    LOG_ERR("MNG", "Empty manga folder");
    return false;
  }
  size_t length = strlen(folder);
  while (length > 1 && folder[length - 1] == '/') --length;
  if (length > std::numeric_limits<size_t>::max() - 257) {
    LOG_ERR("MNG", "Manga folder length overflow");
    return false;
  }
  // Folder lifetime + one reusable full path (FAT filename <=255), independent of depth.
  folder_ = makeUniqueNoThrow<char[]>(length + 1);
  path_ = makeUniqueNoThrow<char[]>(length + 257);
  if (!folder_ || !path_) {
    LOG_ERR("MNG", "OOM for manga paths (%zu bytes)", length * 2 + 258);
    return false;
  }
  memcpy(folder_.get(), folder, length);
  folderLength_ = length;
  return true;
}
const char* MangaBook::path(const char* name) {
  memcpy(path_.get(), folder_.get(), folderLength_);
  size_t offset = folderLength_;
  if (path_[offset - 1] != '/') path_[offset++] = '/';
  strcpy(path_.get() + offset, name);
  return path_.get();
}
bool MangaBook::open(const char* folder, const OpenMode mode, CooperativeCancellation cancellation) {
  close();
  if (cancellation.requested() || !setFolder(folder) || !loadIndex(mode == OpenMode::Reader, cancellation) ||
      ((mode == OpenMode::Reader || mode == OpenMode::Cover) && storageFormat_ == StorageFormat::Legacy &&
       !discoverImages(cancellation)) ||
      cancellation.requested()) {
    close();
    return false;
  }
  if (mode == OpenMode::Reader || mode == OpenMode::Metadata) loadMeta();
  if (mode == OpenMode::Reader) loadToc();
  return true;
}
std::string_view MangaBook::title() const {
  if (!meta_.title.empty()) return meta_.title;
  if (!folder_) return {};
  const char* base = strrchr(folder_.get(), '/');
  return base && base[1] ? std::string_view(base + 1) : std::string_view(folder_.get());
}
bool MangaBook::loadIndex(const bool allocatePage, CooperativeCancellation cancellation) {
  if (cancellation.requested()) return false;
  storageFormat_ = Storage.exists(path("book.mki")) ? StorageFormat::Mokuro : StorageFormat::Legacy;
  const bool mokuro = storageFormat_ == StorageFormat::Mokuro;
  const char* const dataName = mokuro ? "book.mkd" : "panels.dat";
  const char* const indexName = mokuro ? "book.mki" : "panels.idx";
  const size_t headerBytes = mokuro ? kMokuroIndexHeaderBytes : format::kIndexHeaderBytes;
  const size_t recordBytes = mokuro ? kMokuroIndexRecordBytes : format::kIndexRecordBytes;
  FsFile file;
  const char* dataPath = path(dataName);
  if (Storage.exists(dataPath) || mokuro) {
    if (cancellation.requested() || !openFile(dataPath, file)) return false;
    dataSize_ = file.fileSize64();
    if (!finish(file, true, dataName)) return false;
  }
  if (cancellation.requested() || !openFile(path(indexName), file)) return false;
  uint8_t bytes[kMokuroIndexRecordBytes];
  format::IndexHeader header;
  const uint64_t indexSize = file.fileSize64();
  bool ok = read(file, bytes, headerBytes) &&
            (mokuro ? decodeMokuroHeader(bytes, indexSize, header)
                    : format::decodeIndexHeader({bytes, headerBytes}, header) == format::Error::None);
  if (ok && mokuro) mokuroVersion_ = u32(bytes + 4);
  uint64_t expectedDataOffset = 0;
  for (uint32_t i = 0; ok && i < header.pageCount; ++i) {
    format::IndexRecord record;
    ok = !cancellation.requested() && read(file, bytes, recordBytes) &&
         (mokuro ? decodeMokuroRecord(bytes, dataSize_, mokuroVersion_, record)
                 : format::decodeIndexRecord({bytes, recordBytes}, dataSize_, record) == format::Error::None);
    if (ok && mokuro) {
      ok = record.dataOffset == expectedDataOffset;
      expectedDataOffset += record.dataLength;
    }
    if (ok) {
      const uint32_t capacity = record.dataLength + (mokuro ? kMokuroAdapterHeaderBytes : 0u);
      pageCapacity_ = std::max(pageCapacity_, capacity);
    }
  }
  if (mokuro) ok = ok && expectedDataOffset == dataSize_;
  if (!finish(file, ok, indexName) || cancellation.requested()) return false;
  // One reusable buffer per book, <=32768. CMI records reserve their 14-byte
  // legacy PageView prefix in this allocation, so page loads need no second buffer.
  if (pageCapacity_ && allocatePage) {
    page_ = makeUniqueNoThrow<uint8_t[]>(pageCapacity_);
    if (!page_) {
      LOG_ERR("MNG", "OOM for manga page (%u bytes)", unsigned(pageCapacity_));
      return false;
    }
  }
  pageCount_ = header.pageCount;
  return true;
}
bool MangaBook::readPageInfo(uint32_t index, format::IndexRecord& out) {
  out = {};
  if (index >= pageCount_) return false;
  const bool mokuro = storageFormat_ == StorageFormat::Mokuro;
  const char* const indexName = mokuro ? "book.mki" : "panels.idx";
  const size_t headerBytes = mokuro ? kMokuroIndexHeaderBytes : format::kIndexHeaderBytes;
  const size_t recordBytes = mokuro ? kMokuroIndexRecordBytes : format::kIndexRecordBytes;
  FsFile file;
  if (!openFile(path(indexName), file)) return false;
  uint8_t bytes[kMokuroIndexRecordBytes];
  format::IndexRecord record;
  const bool decoded =
      file.seek64(headerBytes + uint64_t(index) * recordBytes) && read(file, bytes, recordBytes) &&
      (mokuro ? decodeMokuroRecord(bytes, dataSize_, mokuroVersion_, record)
              : format::decodeIndexRecord({bytes, recordBytes}, dataSize_, record) == format::Error::None);
  const bool ok = decoded && record.dataLength + (mokuro ? kMokuroAdapterHeaderBytes : 0u) <= pageCapacity_;
  if (!finish(file, ok, indexName)) return false;
  out = record;
  return true;
}
bool MangaBook::loadPage(uint32_t index, format::PageView& out) {
  out = {};
  format::IndexRecord record;
  if (!readPageInfo(index, record)) return false;
  if (!record.dataLength) return true;
  if (!page_) {
    LOG_ERR("MNG", "Page decoding requires reader mode");
    return false;
  }
  const bool mokuro = storageFormat_ == StorageFormat::Mokuro;
  const char* const dataName = mokuro ? "book.mkd" : "panels.dat";
  FsFile file;
  if (!openFile(path(dataName), file)) return false;
  uint8_t* const destination = page_.get() + (mokuro ? kMokuroAdapterHeaderBytes : 0u);
  const bool ok = file.seek64(record.dataOffset) && read(file, destination, record.dataLength);
  if (!finish(file, ok, dataName)) return false;
  uint32_t viewLength = record.dataLength;
  if (mokuro && !adaptMokuroPage(page_.get(), record.dataLength, record, viewLength)) {
    LOG_ERR("MNG", "Mokuro page %u is malformed", unsigned(index));
    return false;
  }
  const auto error = format::decodePage({page_.get(), viewLength}, out);
  if (error != format::Error::None) LOG_ERR("MNG", "Page %u: %s", unsigned(index), format::errorName(error));
  return error == format::Error::None;
}
void MangaBook::loadMeta() {
  const char* filename = path("meta.bin");
  if (!Storage.exists(filename)) return;
  FsFile file;
  if (!openFile(filename, file)) return;
  uint8_t header[8];
  const uint64_t size = file.fileSize64();
  bool ok = read(file, header, sizeof(header)) && u32(header) == 1;
  uint32_t extent = ok ? 8u + u16(header + 4) + u16(header + 6) : 0;
  ok = ok && extent <= size;
  if (ok && size > extent) {
    uint8_t trailer[2];
    ok = size - extent >= 2 && file.seek64(extent) && read(file, trailer, 2);
    if (ok) {
      extent += 2u + u16(trailer);
      ok = extent <= size;
    }
  }
  if (ok) {
    // Only declared, validated bytes; <=196615. Optional OOM falls back to folder title.
    metaBytes_ = makeUniqueNoThrow<uint8_t[]>(extent);
    if (!metaBytes_) LOG_ERR("MNG", "OOM for optional manga metadata (%u bytes)", unsigned(extent));
    ok = metaBytes_ && file.seek64(0) && read(file, metaBytes_.get(), extent);
  }
  format::MetaView meta;
  if (ok) ok = format::decodeMeta({metaBytes_.get(), extent}, meta) == format::Error::None;
  if (!finish(file, ok, "meta.bin (optional; folder-title fallback)")) {
    metaBytes_.reset();
    return;
  }
  meta_ = meta;
}
void MangaBook::loadToc() {
  const char* filename = path("toc.idx");
  if (!Storage.exists(filename)) return;
  FsFile file;
  if (!openFile(filename, file)) return;
  uint8_t header[8];
  bool ok = read(file, header, sizeof(header)) && u32(header) == 1;
  const uint32_t count = ok ? u32(header + 4) : 0;
  ok = ok && count <= format::kMaxTocEntries;
  const uint64_t size = file.fileSize64();
  uint64_t offset = 8;
  uint16_t capacity = 0;
  for (uint32_t i = 0; ok && i < count; ++i) {
    ok = offset <= size && size - offset >= 6 && file.seek64(offset) && read(file, header, 6);
    if (ok) {
      const uint16_t length = u16(header + 4);
      offset += 6;
      ok = length <= size - offset;
      offset += length;
      capacity = std::max(capacity, length);
    }
  }
  if (!finish(file, ok, "toc.idx (optional; percentage fallback)")) return;
  // One reusable label, <=65535. TOC headers streamed; no huge full-TOC allocation.
  if (capacity) {
    tocLabel_ = makeUniqueNoThrow<char[]>(capacity);
    if (!tocLabel_) {
      LOG_ERR("MNG", "OOM for optional manga chapter label (%u bytes)", unsigned(capacity));
      return;
    }
  }
  tocCapacity_ = capacity;
  tocCount_ = count;
}
bool MangaBook::readTocEntry(uint32_t index, format::TocEntryView& out) {
  out = {};
  if (index >= tocCount_) {
    tocNextOffset_ = 8;
    tocNextIndex_ = 0;
    return false;
  }
  FsFile file;
  if (!openFile(path("toc.idx"), file)) {
    tocNextOffset_ = 8;
    tocNextIndex_ = 0;
    return false;
  }
  uint8_t header[8];
  bool ok = read(file, header, 8) && u32(header) == 1 && u32(header + 4) == tocCount_;
  uint64_t offset = index >= tocNextIndex_ ? tocNextOffset_ : 8;
  uint32_t firstIndex = index >= tocNextIndex_ ? tocNextIndex_ : 0;
  const uint64_t size = file.fileSize64();
  uint16_t length = 0;
  for (uint32_t i = firstIndex; ok && i <= index; ++i) {
    ok = offset <= size && size - offset >= 6 && file.seek64(offset) && read(file, header, 6);
    if (ok) {
      length = u16(header + 4);
      offset += 6;
      ok = length <= size - offset;
      offset += length;
    }
  }
  ok = ok && length <= tocCapacity_ && read(file, tocLabel_.get(), length);
  if (!finish(file, ok, "toc.idx")) {
    tocNextOffset_ = 8;
    tocNextIndex_ = 0;
    return false;
  }
  tocNextOffset_ = offset;
  tocNextIndex_ = index + 1;
  out = {u32(header), length ? std::string_view(tocLabel_.get(), length) : std::string_view{}};
  return true;
}
namespace {
bool contains(const char* name, const char* needle) {
  for (; *name; ++name) {
    size_t i = 0;
    while (needle[i] && name[i] && std::tolower(static_cast<unsigned char>(name[i])) == needle[i]) ++i;
    if (!needle[i]) return true;
  }
  return false;
}
int priority(const char* name) {
  const bool cover = contains(name, "cover");
  const bool copyright = contains(name, "copyright");
  return cover ? (copyright ? 0 : 1) : (copyright ? 2 : 3);
}
int compare(const char* a, uint64_t aOrdinal, const char* b, uint64_t bOrdinal) {
  const int ap = priority(a), bp = priority(b);
  if (ap != bp) return ap - bp;
  if (ap == 3) {
    const int order = FsHelpers::naturalCompare(a, b);
    if (order) return order;
  }
  return aOrdinal < bOrdinal ? -1 : (aOrdinal > bOrdinal ? 1 : 0);
}
bool cropName(const char* name) {
  if (*name != 'p' && *name != 'P') return false;
  const char* p = name + 1;
  if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
  while (std::isdigit(static_cast<unsigned char>(*p))) ++p;
  if (*p++ != '_') return false;
  if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
  while (std::isdigit(static_cast<unsigned char>(*p))) ++p;
  return *p == '.';
}
bool pageName(const char* name) {
  if (!*name || *name == '.' || cropName(name)) return false;
  const char* ext = strrchr(name, '.');
  if (!ext) return false;
  return !FsHelpers::naturalCompare(ext, ".jpg") || !FsHelpers::naturalCompare(ext, ".jpeg") ||
         !FsHelpers::naturalCompare(ext, ".bmp") || !FsHelpers::naturalCompare(ext, ".png");
}
}  // namespace
bool MangaBook::isMangaFolder(const char* folder) {
  MangaBook probe;
  if (!probe.setFolder(folder)) return false;
  const auto mokuro = probe.probeFile("book.mki");
  if (mokuro == PathResult::Found) return true;
  return mokuro != PathResult::Error && probe.probeFile("panels.idx") == PathResult::Found;
}
PathResult MangaBook::probeFile(const char* name) {
  const char* filename = path(name);
  if (!Storage.exists(filename)) return PathResult::Missing;
  FsFile file = Storage.open(filename);
  if (!file) {
    file.close();
    LOG_ERR("MNG", "Cannot probe %s", filename);
    return PathResult::Error;
  }
  const bool directory = file.isDirectory();
  if (!finish(file, true, filename)) return PathResult::Error;
  return directory ? PathResult::Missing : PathResult::Found;
}
bool MangaBook::discoverImages(CooperativeCancellation cancellation) {
  char name[32];
  static constexpr const char* extensions[] = {".jpg", ".jpeg", ".bmp", ".png"};
  // Browser panels-only output always retains page zero, but may omit any
  // other full image and retain a different extension for each physical page.
  for (const char* ext : extensions) {
    if (cancellation.requested()) return false;
    snprintf(name, sizeof(name), "page_%04u%s", 0u, ext);
    const auto first = probeFile(name);
    if (first == PathResult::Error) return false;
    if (first == PathResult::Found) {
      canonicalExtension_ = ext;
      break;
    }
  }
  if (cancellation.requested()) return false;
  const char* directory = path("panels");
  if (Storage.exists(directory)) {
    FsFile file = Storage.open(directory);
    const bool ok = bool(file);
    if (ok) panelsDirectory_ = file.isDirectory();
    if (!finish(file, ok, directory)) return false;
  }
  return true;
}
bool MangaBook::classifyImages(CooperativeCancellation cancellation) {
  char name[32];
  static constexpr const char* extensions[] = {".jpg", ".jpeg", ".bmp", ".png"};
  if (!canonicalExtension_) {
    // Missing cover: classify once, never compact canonical holes into legacy
    // positions. Reuse the same bounded scan owner as legacy navigation.
    imageScan_ = makeUniqueNoThrow<ImageScan>();
    if (!imageScan_) {
      LOG_ERR("MNG", "OOM for manga image directory scan");
      return false;
    }
    if (cancellation.requested()) return false;
    FsFile directory = Storage.open(folder_.get());
    if (!directory || !directory.isDirectory()) return finish(directory, false, folder_.get());
    bool ok = true;
    while (true) {
      if (cancellation.requested()) {
        ok = false;
        break;
      }
      FsFile entry = directory.openNextFile();
      if (!entry) {
        ok = !directory.allocationFailed() && !entry.allocationFailed();
        entry.close();
        break;
      }
      bool canonical = false;
      if (!entry.isDirectory()) {
        const size_t length = entry.getName(imageScan_->entry, sizeof(imageScan_->entry));
        ok = length > 0 && length < sizeof(imageScan_->entry);
        if (ok && strncmp(imageScan_->entry, "page_", 5) == 0) {
          const char* digits = imageScan_->entry + 5;
          uint64_t index = 0;
          while (*digits >= '0' && *digits <= '9' && index <= UINT32_MAX)
            index = index * 10 + unsigned(*digits++ - '0');
          if (index < pageCount_) {
            for (const char* ext : extensions) {
              snprintf(name, sizeof(name), "page_%04u%s", unsigned(index), ext);
              if (strcmp(imageScan_->entry, name) == 0) {
                canonicalExtension_ = ext;
                canonical = true;
                break;
              }
            }
          }
        }
      }
      if (!finish(entry, ok, "manga image classification")) {
        ok = false;
        break;
      }
      if (canonical) break;
    }
    if (!finish(directory, ok, folder_.get())) return false;
  }
  imagesClassified_ = true;
  return true;
}
PathResult MangaBook::copyPath(const char* name, char* out, size_t size) {
  const char* resolved = path(name);
  const size_t length = strlen(resolved);
  if (length >= size) {
    LOG_ERR("MNG", "Manga path output too small (%zu required)", length + 1);
    return PathResult::Error;
  }
  memcpy(out, resolved, length + 1);
  return PathResult::Found;
}
PathResult MangaBook::legacyPage(uint32_t index, CooperativeCancellation cancellation) {
  if (!imageScan_) {
    // Three FAT filenames + cursor (~800 bytes), reused across scans, not on task stack.
    imageScan_ = makeUniqueNoThrow<ImageScan>();
    if (!imageScan_) {
      LOG_ERR("MNG", "OOM for manga image directory scan");
      return PathResult::Error;
    }
  }
  auto& scan = *imageScan_;
  if (scan.hasLast && index < scan.index) scan.hasLast = false;
  while (!scan.hasLast || scan.index < index) {
    if (cancellation.requested()) return PathResult::Error;
    FsFile directory = Storage.open(folder_.get());
    if (!directory || !directory.isDirectory()) {
      finish(directory, false, folder_.get());
      return PathResult::Error;
    }
    bool ok = true, found = false;
    uint64_t ordinal = 0, bestOrdinal = 0;
    while (true) {
      if (cancellation.requested()) {
        ok = false;
        break;
      }
      FsFile entry = directory.openNextFile();
      if (!entry) {
        ok = !directory.allocationFailed() && !entry.allocationFailed();
        entry.close();
        break;
      }
      bool candidate = false;
      if (!entry.isDirectory()) {
        const size_t length = entry.getName(scan.entry, sizeof(scan.entry));
        ok = length > 0 && length < sizeof(scan.entry);
        candidate = ok && pageName(scan.entry);
      }
      if (!finish(entry, ok, "manga directory entry")) {
        ok = false;
        break;
      }
      if (candidate && (!scan.hasLast || compare(scan.entry, ordinal, scan.last, scan.lastOrdinal) > 0) &&
          (!found || compare(scan.entry, ordinal, scan.best, bestOrdinal) < 0)) {
        strcpy(scan.best, scan.entry);
        bestOrdinal = ordinal;
        found = true;
      }
      ++ordinal;
    }
    if (!finish(directory, ok, folder_.get())) return PathResult::Error;
    if (!found) return PathResult::Missing;
    strcpy(scan.last, scan.best);
    scan.lastOrdinal = bestOrdinal;
    scan.index = scan.hasLast ? scan.index + 1 : 0;
    scan.hasLast = true;
  }
  return probeFile(scan.last);
}
PathResult MangaBook::pageImagePath(uint32_t index, char* out, size_t size, CooperativeCancellation cancellation) {
  if (out && size) *out = 0;
  if (cancellation.requested() || !out || !size || index >= pageCount_) return PathResult::Error;
  if (storageFormat_ == StorageFormat::Mokuro) {
    char name[32];
    snprintf(name, sizeof(name), "page_%04u.bmp", unsigned(index));
    const auto result = probeFile(name);
    return result == PathResult::Found ? copyPath(name, out, size) : result;
  }
  if (!canonicalExtension_ && !imagesClassified_ && !classifyImages(cancellation)) return PathResult::Error;
  if (canonicalExtension_) {
    char name[32];
    // Deterministic duplicate priority is per physical page, not per book.
    static constexpr const char* extensions[] = {".jpg", ".jpeg", ".bmp", ".png"};
    for (const char* ext : extensions) {
      if (cancellation.requested()) return PathResult::Error;
      snprintf(name, sizeof(name), "page_%04u%s", unsigned(index), ext);
      const auto result = probeFile(name);
      if (result == PathResult::Found) return copyPath(name, out, size);
      if (result == PathResult::Error) return result;
    }
    return PathResult::Missing;
  }
  const auto result = legacyPage(index, cancellation);
  return result == PathResult::Found ? copyPath(imageScan_->last, out, size) : result;
}
PathResult MangaBook::panelImagePath(uint32_t page, uint16_t panel, char* out, size_t size) {
  if (out && size) *out = 0;
  if (!out || !size || page >= pageCount_) return PathResult::Error;
  if (storageFormat_ == StorageFormat::Mokuro) return PathResult::Missing;
  char name[40];
  static constexpr const char* extensions[] = {".bmp", ".jpg"};
  for (const char* ext : extensions) {
    snprintf(name, sizeof(name), "%sp%u_%u%s", panelsDirectory_ ? "panels/" : "", unsigned(page), unsigned(panel), ext);
    const auto result = probeFile(name);
    if (result == PathResult::Error) return result;
    if (result == PathResult::Found) return copyPath(name, out, size);
  }
  return PathResult::Missing;
}
}  // namespace manga
