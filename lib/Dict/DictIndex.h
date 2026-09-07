#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "DictionaryScanIdentity.h"

enum class JapaneseDictStatus : uint8_t {
  Found,
  NotFound,
  Unavailable,
  ReadError,
  OutOfMemory,
};

struct DictIndexRecord {
  static constexpr size_t HEADWORD_SIZE = 32;

  static constexpr uint8_t POS_V1 = 0x01;
  static constexpr uint8_t POS_V5 = 0x02;
  static constexpr uint8_t POS_VS = 0x04;
  static constexpr uint8_t POS_VK = 0x08;
  static constexpr uint8_t POS_ADJ_I = 0x10;
  static constexpr uint8_t POS_OTHER = 0x20;
  static constexpr uint8_t POS_READING = 0x40;

  char headword[HEADWORD_SIZE];
  uint32_t offset;
  uint16_t length;
  uint8_t priority;
  uint8_t posFlags;
} __attribute__((packed));
static_assert(sizeof(DictIndexRecord) == 40);

struct DictProbe {
  char headword[DictIndexRecord::HEADWORD_SIZE] = {};
  uint8_t priority = 0;
  uint8_t sourceDict = 0;
  uint8_t posFlags = 0;
};

struct DictEntry {
  char headword[DictIndexRecord::HEADWORD_SIZE] = {};
  uint8_t headwordLength = 0;
  std::unique_ptr<char[]> definition;
  size_t definitionLength = 0;
  uint8_t priority = 0;
  uint8_t sourceDict = 0;
  uint8_t posFlags = 0;

  DictEntry() = default;
  DictEntry(DictEntry&&) noexcept = default;
  DictEntry& operator=(DictEntry&&) noexcept = default;
  DictEntry(const DictEntry&) = delete;
  DictEntry& operator=(const DictEntry&) = delete;

  // Views remain valid until this value is moved, reset, or reused by lookupExact().
  std::string_view headwordView() const { return {headword, headwordLength}; }
  std::string_view definitionView() const {
    return definition ? std::string_view(definition.get(), definitionLength) : std::string_view{};
  }
  void reset() {
    headword[0] = '\0';
    headwordLength = 0;
    definition.reset();
    definitionLength = 0;
    priority = 0;
    sourceDict = 0;
    posFlags = 0;
  }
};

class DictIndex {
 public:
  static constexpr uint8_t DICT_JMDICT = 1;
  static constexpr uint8_t DICT_GRAMMAR = 2;
  static constexpr uint8_t DICT_NAMES = 4;
  static constexpr uint8_t DICT_ALL = DICT_JMDICT | DICT_GRAMMAR | DICT_NAMES;

  DictIndex();
  ~DictIndex();
  DictIndex(const DictIndex&) = delete;
  DictIndex& operator=(const DictIndex&) = delete;

  JapaneseDictStatus open();
  JapaneseDictStatus probeExact(std::string_view headword, DictProbe& out, uint8_t dictMask = DICT_ALL,
                                uint8_t posMask = 0);
  JapaneseDictStatus lookupExact(std::string_view headword, DictEntry& out, uint8_t dictMask = DICT_ALL,
                                 uint8_t posMask = 0);
  uint8_t availableSources() const;
  uint64_t signature() const;
  DictionaryScanIdentityStatus beginScanIdentity(DictionaryScanIdentityState& state);
  DictionaryScanIdentityStatus stepScanIdentity(DictionaryScanIdentityState& state, size_t byteBudget);
  bool resumeScanIdentity(DictionaryScanIdentityState& state);
  void close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  uint8_t availableSources_ = 0;
  uint64_t signature_ = 0;
};
