#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "Deinflector.h"
#include "DictIndex.h"

struct WordLookupProbe {
  bool usuallyKana = false;
  bool markerCheckFailed = false;
  size_t matchLength = 0;
  bool deinflected = false;
  uint8_t sourceDict = 0;
  uint8_t priority = 0;
  uint8_t posFlags = 0;
};

struct WordLookupResult {
  DictEntry entry;
  size_t matchLength = 0;
  bool deinflected = false;
};

class WordLookup {
 public:
  static constexpr uint8_t MAX_WINDOW_CHARS = 8;

  explicit WordLookup(DictIndex& index) : index_(index) {}
  JapaneseDictStatus probe(std::string_view text, size_t byteOffset, WordLookupProbe& out);
  JapaneseDictStatus lookup(std::string_view text, size_t byteOffset, WordLookupResult& out);

 private:
  struct Match {
    char headword[DictIndexRecord::HEADWORD_SIZE] = {};
    uint8_t headwordLength = 0;
    uint8_t dictMask = 0;
    uint8_t posMask = 0;
    size_t matchLength = 0;
    bool deinflected = false;
    uint8_t priority = 0;
    uint8_t posFlags = 0;
  };

  JapaneseDictStatus find(std::string_view text, size_t byteOffset, Match& match);

  DictIndex& index_;
  // About 2.2 KiB, owned with the lookup session and reused for every probe.
  // A task-stack buffer is too large and static storage would prevent independent sessions.
  DeinflectionBuffer candidates_{};
};
