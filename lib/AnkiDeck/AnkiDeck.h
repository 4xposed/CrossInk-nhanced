#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "AnkiDeckTypes.h"

struct AnkiDeckMetadata {
  uint64_t sourceCardId = 0;
  uint32_t promptOffset = 0;
  uint16_t promptLength = 0;
  uint32_t answerOffset = 0;
  uint16_t answerLength = 0;
  uint16_t importedIntervalDays = 0;
  uint8_t initialKind = 0;
};

class ReviewStateStore;

class AnkiDeck {
 public:
  bool load(const std::string& path);
  const char* title() const;
  uint32_t cardCount() const;
  bool readCardFields(uint32_t index, CardFields& out);
  const std::string& getPath() const;
  const std::string& getCachePath() const;

 private:
  using CardMetadata = AnkiDeckMetadata;

  // Bounded by the v1 u16 title length; this is the only deck-owned heap data.
  std::unique_ptr<char[]> title_;
  std::string path_;
  std::string cachePath_;
  uint64_t deckId_ = 0;
  uint32_t cardCount_ = 0;
  uint32_t indexOffset_ = 0;
  uint32_t textOffset_ = 0;
  uint16_t version_ = 0;
  uint64_t fileSize_ = 0;

  using MetadataVisitor = bool (*)(uint32_t index, const CardMetadata& metadata, void* context);

  bool streamMetadata(MetadataVisitor visitor, void* context) const;
  bool validateFullIndex() const;

  friend class ReviewStateStore;
};
