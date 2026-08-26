#include "AnkiDeck.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
constexpr char kLogTag[] = "AnkiDeck";
constexpr uint32_t kHeaderSize = 32;
constexpr uint32_t kIndexSize = 29;
constexpr uint32_t kMaxCards = 4096;
constexpr uint16_t kV2IndexSize = kIndexSize;
constexpr uint16_t kMaxV2SidePayloadBytes = 1 + kMaxCardFields * (1 + sizeof(uint16_t)) + kMaxCardSideTextBytes;

uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t readLe64(const uint8_t* data) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8; ++index) {
    value |= static_cast<uint64_t>(data[index]) << (index * 8);
  }
  return value;
}

bool readExactly(FsFile& file, void* buffer, size_t size) { return file.read(buffer, size) == static_cast<int>(size); }

bool validUtf8(const uint8_t* bytes, size_t size) {
  for (size_t index = 0; index < size;) {
    const uint8_t first = bytes[index++];
    if (first <= 0x7F) {
      continue;
    }
    uint32_t codepoint = 0;
    uint8_t trailing = 0;
    if (first >= 0xC2 && first <= 0xDF) {
      codepoint = first & 0x1F;
      trailing = 1;
    } else if (first >= 0xE0 && first <= 0xEF) {
      codepoint = first & 0x0F;
      trailing = 2;
    } else if (first >= 0xF0 && first <= 0xF4) {
      codepoint = first & 0x07;
      trailing = 3;
    } else {
      return false;
    }
    if (index + trailing > size) {
      return false;
    }
    for (uint8_t trailingIndex = 0; trailingIndex < trailing; ++trailingIndex) {
      const uint8_t next = bytes[index++];
      if ((next & 0xC0) != 0x80) {
        return false;
      }
      codepoint = (codepoint << 6) | (next & 0x3F);
    }
    if ((trailing == 2 && codepoint < 0x800) || (trailing == 3 && codepoint < 0x10000) ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF) || codepoint > 0x10FFFF) {
      return false;
    }
  }
  return true;
}

uint64_t fnv1a64(const std::string& value) {
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

void decodeMetadata(const uint8_t* bytes, AnkiDeckMetadata& out) {
  out.sourceCardId = readLe64(bytes);
  out.promptOffset = readLe32(bytes + 8);
  out.promptLength = readLe16(bytes + 12);
  out.answerOffset = readLe32(bytes + 14);
  out.answerLength = readLe16(bytes + 18);
  out.importedIntervalDays = readLe16(bytes + 24);
  out.initialKind = bytes[28];
}

bool readNextMetadataFromFile(FsFile& file, AnkiDeckMetadata& out) {
  std::array<uint8_t, kIndexSize> bytes{};
  if (!readExactly(file, bytes.data(), bytes.size())) return false;
  decodeMetadata(bytes.data(), out);
  return true;
}

bool readMetadataFromFile(FsFile& file, uint32_t indexOffset, uint32_t index, AnkiDeckMetadata& out) {
  const uint64_t position = static_cast<uint64_t>(indexOffset) + static_cast<uint64_t>(index) * kIndexSize;
  return file.seek64(position) && readNextMetadataFromFile(file, out);
}

bool validMetadata(const AnkiDeckMetadata& metadata, uint16_t version, uint32_t textOffset, uint64_t fileSize) {
  const uint16_t maxSideLength = version == 1 ? kMaxCardFieldTextBytes : kMaxV2SidePayloadBytes;
  if (metadata.sourceCardId == 0 || metadata.promptLength == 0 || metadata.promptLength > maxSideLength ||
      metadata.answerLength == 0 || metadata.answerLength > maxSideLength ||
      metadata.initialKind > static_cast<uint8_t>(ReviewKind::Learning)) {
    return false;
  }
  const uint64_t promptEnd = static_cast<uint64_t>(metadata.promptOffset) + metadata.promptLength;
  const uint64_t answerEnd = static_cast<uint64_t>(metadata.answerOffset) + metadata.answerLength;
  return promptEnd >= metadata.promptOffset && answerEnd >= metadata.answerOffset &&
         static_cast<uint64_t>(textOffset) + promptEnd <= fileSize &&
         static_cast<uint64_t>(textOffset) + answerEnd <= fileSize;
}

bool validUtf8FileRange(FsFile& file, uint64_t start, uint16_t length) {
  if (!file.seek64(start)) return false;

  std::array<uint8_t, 64> bytes{};
  uint16_t remaining = length;
  uint8_t trailing = 0;
  uint32_t codepoint = 0;
  uint32_t minimum = 0;
  while (remaining > 0) {
    const uint16_t count = std::min<uint16_t>(remaining, bytes.size());
    if (!readExactly(file, bytes.data(), count)) return false;
    remaining -= count;
    for (uint16_t index = 0; index < count; ++index) {
      const uint8_t byte = bytes[index];
      if (trailing == 0) {
        if (byte <= 0x7F) continue;
        if (byte >= 0xC2 && byte <= 0xDF) {
          codepoint = byte & 0x1F;
          minimum = 0x80;
          trailing = 1;
        } else if (byte >= 0xE0 && byte <= 0xEF) {
          codepoint = byte & 0x0F;
          minimum = 0x800;
          trailing = 2;
        } else if (byte >= 0xF0 && byte <= 0xF4) {
          codepoint = byte & 0x07;
          minimum = 0x10000;
          trailing = 3;
        } else {
          return false;
        }
        continue;
      }
      if ((byte & 0xC0) != 0x80) return false;
      codepoint = (codepoint << 6) | (byte & 0x3F);
      --trailing;
      if (trailing == 0 &&
          (codepoint < minimum || (codepoint >= 0xD800 && codepoint <= 0xDFFF) || codepoint > 0x10FFFF)) {
        return false;
      }
    }
  }
  return trailing == 0;
}

// v2 side payload: u8 block count, then u8 flags, u16 LE text length, and UTF-8 bytes per block.
bool validV2Side(FsFile& file, uint64_t start, uint16_t length) {
  if (length < 1 || length > kMaxV2SidePayloadBytes || !file.seek64(start)) return false;

  uint8_t count = 0;
  if (!readExactly(file, &count, sizeof(count)) || count == 0 || count > kMaxCardFields) return false;

  uint16_t contentLength = 0;
  uint16_t consumed = 1;
  for (uint8_t index = 0; index < count; ++index) {
    std::array<uint8_t, 3> blockHeader{};
    if (!readExactly(file, blockHeader.data(), blockHeader.size()) || (blockHeader[0] & ~0x01U) != 0) return false;
    const uint16_t blockLength = readLe16(blockHeader.data() + 1);
    if (blockLength == 0 || blockLength > kMaxCardFieldTextBytes ||
        contentLength > kMaxCardSideTextBytes - blockLength ||
        static_cast<uint32_t>(consumed) + blockHeader.size() + blockLength > length) {
      return false;
    }
    contentLength += blockLength;
    consumed += 3 + blockLength;
    const uint64_t textStart = start + consumed - blockLength;
    if (!validUtf8FileRange(file, textStart, blockLength)) return false;
    if (!file.seek64(start + consumed)) return false;
  }
  return consumed == length;
}

bool readV2Side(FsFile& file, uint64_t start, std::array<CardField, kMaxCardFields>& fields, uint8_t& count) {
  if (!file.seek64(start) || !readExactly(file, &count, sizeof(count)) || count == 0 || count > kMaxCardFields) {
    return false;
  }
  for (uint8_t index = 0; index < count; ++index) {
    std::array<uint8_t, 3> blockHeader{};
    if (!readExactly(file, blockHeader.data(), blockHeader.size())) return false;
    const uint16_t length = readLe16(blockHeader.data() + 1);
    if ((blockHeader[0] & ~0x01U) != 0 || length == 0 || length > kMaxCardFieldTextBytes ||
        fields[index].text == nullptr || !readExactly(file, fields[index].text, length) ||
        !validUtf8(reinterpret_cast<const uint8_t*>(fields[index].text), length)) {
      return false;
    }
    fields[index].text[length] = '\0';
    fields[index].length = length;
    fields[index].primary = (blockHeader[0] & 0x01U) != 0;
  }
  return true;
}
}  // namespace

bool AnkiDeck::load(const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead(kLogTag, path, file)) {
    LOG_ERR(kLogTag, "Could not open deck: %s", path.c_str());
    return false;
  }

  const uint64_t fileSize = file.fileSize64();
  std::array<uint8_t, kHeaderSize> header{};
  if (fileSize < header.size() || !readExactly(file, header.data(), header.size())) {
    LOG_ERR(kLogTag, "Deck header is truncated");
    file.close();
    return false;
  }
  const uint16_t version = readLe16(header.data() + 4);
  const uint16_t indexRecordSize = readLe16(header.data() + 22);
  if (std::memcmp(header.data(), "CKDK", 4) != 0 || (version != 1 && version != 2) ||
      readLe16(header.data() + 6) != kHeaderSize ||
      (version == 1 ? indexRecordSize != 0 : indexRecordSize != kV2IndexSize)) {
    LOG_ERR(kLogTag, "Deck header is invalid");
    file.close();
    return false;
  }

  const uint64_t deckId = readLe64(header.data() + 8);
  const uint32_t count = readLe32(header.data() + 16);
  const uint16_t titleLength = readLe16(header.data() + 20);
  const uint32_t indexOffset = readLe32(header.data() + 24);
  const uint32_t textOffset = readLe32(header.data() + 28);
  const uint64_t expectedIndexOffset = static_cast<uint64_t>(kHeaderSize) + titleLength;
  const uint64_t expectedTextOffset = expectedIndexOffset + static_cast<uint64_t>(count) * kIndexSize;
  if (deckId == 0 || count == 0 || count > kMaxCards || indexOffset != expectedIndexOffset ||
      textOffset != expectedTextOffset || textOffset > fileSize) {
    LOG_ERR(kLogTag, "Deck offsets or count are invalid");
    file.close();
    return false;
  }

  // The title is bounded by the v1 u16 field and retained for the deck lifetime.
  auto loadedTitle = makeUniqueNoThrow<char[]>(static_cast<size_t>(titleLength) + 1);
  if (!loadedTitle) {
    LOG_ERR(kLogTag, "OOM allocating deck title: %u bytes", static_cast<unsigned>(titleLength) + 1U);
    file.close();
    return false;
  }
  if (!file.seek64(kHeaderSize) || !readExactly(file, loadedTitle.get(), titleLength) ||
      !validUtf8(reinterpret_cast<const uint8_t*>(loadedTitle.get()), titleLength)) {
    LOG_ERR(kLogTag, "Deck title is invalid UTF-8");
    file.close();
    return false;
  }
  loadedTitle[titleLength] = '\0';

  if (!file.close()) {
    LOG_ERR(kLogTag, "Could not close deck header");
    return false;
  }

  char hash[17]{};
  std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(fnv1a64(path)));
  path_ = path;
  cachePath_ = std::string("/.crosspoint/anki_") + hash;
  title_ = std::move(loadedTitle);
  deckId_ = deckId;
  cardCount_ = count;
  indexOffset_ = indexOffset;
  textOffset_ = textOffset;
  version_ = version;
  fileSize_ = fileSize;
  return true;
}

const char* AnkiDeck::title() const { return title_ ? title_.get() : ""; }
uint32_t AnkiDeck::cardCount() const { return cardCount_; }
const std::string& AnkiDeck::getPath() const { return path_; }
const std::string& AnkiDeck::getCachePath() const { return cachePath_; }

bool AnkiDeck::streamMetadata(MetadataVisitor visitor, void* context) const {
  if (visitor == nullptr) return false;
  FsFile file;
  if (!Storage.openFileForRead(kLogTag, path_, file)) {
    LOG_ERR(kLogTag, "Could not open deck index for streaming");
    return false;
  }
  bool streamed = file.seek64(indexOffset_);
  for (uint32_t index = 0; streamed && index < cardCount_; ++index) {
    CardMetadata metadata;
    streamed = readNextMetadataFromFile(file, metadata) && validMetadata(metadata, version_, textOffset_, fileSize_) &&
               visitor(index, metadata, context);
  }
  const bool closed = file.close();
  if (!streamed || !closed) LOG_ERR(kLogTag, "Could not stream deck index");
  return streamed && closed;
}

bool AnkiDeck::validateFullIndex() const {
  FsFile file;
  if (!Storage.openFileForRead(kLogTag, path_, file)) {
    LOG_ERR(kLogTag, "Could not open deck index for validation");
    return false;
  }
  // Transient 32 KiB maximum source-ID set avoids quadratic SD reads; it is released on return.
  auto sourceIds = makeUniqueNoThrow<uint64_t[]>(cardCount_);
  if (!sourceIds) {
    LOG_ERR(kLogTag, "OOM allocating source ID validation set: %u bytes",
            static_cast<unsigned>(cardCount_ * sizeof(uint64_t)));
    file.close();
    return false;
  }
  if (!file.seek64(indexOffset_)) {
    LOG_ERR(kLogTag, "Could not seek deck index for validation");
    file.close();
    return false;
  }
  for (uint32_t index = 0; index < cardCount_; ++index) {
    CardMetadata metadata;
    const bool metadataValid = readMetadataFromFile(file, indexOffset_, index, metadata) &&
                               validMetadata(metadata, version_, textOffset_, fileSize_);
    const bool fieldsValid =
        metadataValid &&
        (version_ == 1 ||
         (validV2Side(file, static_cast<uint64_t>(textOffset_) + metadata.promptOffset, metadata.promptLength) &&
          validV2Side(file, static_cast<uint64_t>(textOffset_) + metadata.answerOffset, metadata.answerLength)));
    if (!fieldsValid) {
      LOG_ERR(kLogTag, "Deck index record is invalid");
      file.close();
      return false;
    }
    sourceIds[index] = metadata.sourceCardId;
  }
  std::sort(sourceIds.get(), sourceIds.get() + cardCount_);
  for (uint32_t index = 1; index < cardCount_; ++index) {
    if (sourceIds[index - 1] == sourceIds[index]) {
      LOG_ERR(kLogTag, "Deck contains duplicate source card IDs");
      file.close();
      return false;
    }
  }
  const bool closed = file.close();
  if (!closed) LOG_ERR(kLogTag, "Could not close validated deck index");
  return closed;
}

bool AnkiDeck::readCardFields(uint32_t index, CardFields& out) {
  out.promptCount = 0;
  out.answerCount = 0;
  if (index >= cardCount_) return false;

  FsFile file;
  if (!Storage.openFileForRead(kLogTag, path_, file)) {
    LOG_ERR(kLogTag, "Could not open deck card fields");
    return false;
  }

  CardMetadata metadata;
  const bool metadataRead = readMetadataFromFile(file, indexOffset_, index, metadata) &&
                            validMetadata(metadata, version_, textOffset_, fileSize_);
  bool fieldsRead = metadataRead;
  if (fieldsRead && version_ == 1) {
    fieldsRead = out.prompt[0].text != nullptr && out.answer[0].text != nullptr &&
                 file.seek64(static_cast<uint64_t>(textOffset_) + metadata.promptOffset) &&
                 readExactly(file, out.prompt[0].text, metadata.promptLength) &&
                 file.seek64(static_cast<uint64_t>(textOffset_) + metadata.answerOffset) &&
                 readExactly(file, out.answer[0].text, metadata.answerLength);
    if (fieldsRead) {
      out.prompt[0].text[metadata.promptLength] = '\0';
      out.prompt[0].length = metadata.promptLength;
      out.prompt[0].primary = true;
      out.answer[0].text[metadata.answerLength] = '\0';
      out.answer[0].length = metadata.answerLength;
      out.answer[0].primary = true;
      out.promptCount = 1;
      out.answerCount = 1;
    }
  } else if (fieldsRead) {
    const uint64_t promptStart = static_cast<uint64_t>(textOffset_) + metadata.promptOffset;
    const uint64_t answerStart = static_cast<uint64_t>(textOffset_) + metadata.answerOffset;
    fieldsRead = validV2Side(file, promptStart, metadata.promptLength) &&
                 validV2Side(file, answerStart, metadata.answerLength) &&
                 readV2Side(file, promptStart, out.prompt, out.promptCount) &&
                 readV2Side(file, answerStart, out.answer, out.answerCount);
  }
  const bool closed = file.close();
  if (!fieldsRead || !closed) {
    out.promptCount = 0;
    out.answerCount = 0;
    LOG_ERR(kLogTag, "Could not read deck card fields");
    return false;
  }
  return true;
}
