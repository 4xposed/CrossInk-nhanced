#pragma once

#include <Memory.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

enum class DictionaryBackendKind : uint8_t { StarDict, Japanese };

enum class DictionaryStatus : uint8_t { Found, NotFound, Unavailable, ReadError, Cancelled, OutOfMemory };

enum class DictionaryLookupMode : uint8_t { Token, LongestAtOffset };

class DictionaryOwnedText {
 public:
  DictionaryOwnedText() = default;
  DictionaryOwnedText(const DictionaryOwnedText&) = delete;
  DictionaryOwnedText& operator=(const DictionaryOwnedText&) = delete;
  DictionaryOwnedText(DictionaryOwnedText&& other) noexcept
      : data_(std::move(other.data_)),
        length_(std::exchange(other.length_, 0)),
        capacity_(std::exchange(other.capacity_, 0)) {}
  DictionaryOwnedText& operator=(DictionaryOwnedText&& other) noexcept {
    if (this == &other) return *this;
    data_ = std::move(other.data_);
    length_ = std::exchange(other.length_, 0);
    capacity_ = std::exchange(other.capacity_, 0);
    return *this;
  }

  bool assign(const std::string_view value) {
    if (value.empty()) {
      reset();
      return true;
    }
    if (value.size() == SIZE_MAX) return false;
    const size_t required = value.size() + 1;
    if (required > capacity_) {
      auto replacement = makeUniqueNoThrow<char[]>(required);
      if (!replacement) return false;
      std::memcpy(replacement.get(), value.data(), value.size());
      replacement[value.size()] = '\0';
      data_ = std::move(replacement);
      capacity_ = required;
    } else {
      std::memmove(data_.get(), value.data(), value.size());
      data_[value.size()] = '\0';
    }
    length_ = value.size();
    return true;
  }

  bool assignJoined(const std::string_view first, const std::string_view second) {
    if (first.size() > SIZE_MAX - second.size() || first.size() + second.size() == SIZE_MAX) return false;
    const size_t joinedLength = first.size() + second.size();
    if (joinedLength == 0) {
      reset();
      return true;
    }
    const size_t required = joinedLength + 1;
    // Joining may alias the current value, so construct one exact replacement
    // and publish it only after the fallible allocation succeeds.
    auto replacement = makeUniqueNoThrow<char[]>(required);
    if (!replacement) return false;
    if (!first.empty()) std::memcpy(replacement.get(), first.data(), first.size());
    // cppcheck-suppress arithOperationsOnVoidPointer
    if (!second.empty()) std::memcpy(replacement.get() + first.size(), second.data(), second.size());
    replacement[joinedLength] = '\0';
    data_ = std::move(replacement);
    length_ = joinedLength;
    capacity_ = required;
    return true;
  }

  // Adopt an exact NUL-terminated fallible allocation without copying it.
  // Used when a backend has already built a transactional result buffer.
  bool adopt(std::unique_ptr<char[]> value, const size_t length, const size_t capacity) {
    if (length == 0) {
      reset();
      return true;
    }
    if (!value || capacity <= length || value[length] != '\0') return false;
    data_ = std::move(value);
    length_ = length;
    capacity_ = capacity;
    return true;
  }

  void reset() {
    data_.reset();
    length_ = 0;
    capacity_ = 0;
  }

  std::string_view view() const { return data_ ? std::string_view(data_.get(), length_) : std::string_view{}; }
  const char* c_str() const { return data_ ? data_.get() : ""; }
  size_t length() const { return length_; }
  size_t capacity() const { return capacity_; }
  bool empty() const { return length_ == 0; }

 private:
  std::unique_ptr<char[]> data_;
  size_t length_ = 0;
  size_t capacity_ = 0;
};

struct DictionaryCapabilities {
  bool suggestions = false;
  bool stemVariants = false;
  bool dictionarySwitch = false;
  bool deinflection = false;
  bool names = false;
  bool grammar = false;
  bool ruby = false;
};

struct DictionaryQuery {
  // Intentional implicit conversion preserves the compact `lookup({"word"})`
  // call boundary used by both backends and their native failure-injection tests.
  // cppcheck-suppress noExplicitConstructor
  constexpr DictionaryQuery(std::string_view text = {}, size_t byteOffset = 0,
                            DictionaryLookupMode mode = DictionaryLookupMode::Token,
                            bool synthesizePartialKatakanaName = false, std::string_view syntheticNameDefinition = {})
      : text(text),
        byteOffset(byteOffset),
        mode(mode),
        synthesizePartialKatakanaName(synthesizePartialKatakanaName),
        syntheticNameDefinition(syntheticNameDefinition) {}

  std::string_view text;
  size_t byteOffset = 0;
  DictionaryLookupMode mode = DictionaryLookupMode::Token;
  bool synthesizePartialKatakanaName = false;
  std::string_view syntheticNameDefinition;
};

struct DictionaryProbeResult {
  static constexpr uint8_t kReadingRecord = 0x40;

  DictionaryStatus status = DictionaryStatus::NotFound;
  size_t matchedBytes = 0;
  bool transformed = false;
  uint8_t sourceMask = 0;
  // Definition-free Japanese scan metadata. StarDict leaves both neutral.
  uint8_t priority = 0;
  uint8_t posFlags = 0;
};

struct DictionaryDefinitionHandle {
  uint32_t generation = 0;
  uint32_t epoch = 0;
};

struct DictionarySuggestions {
  static constexpr uint8_t kCapacity = 8;
  std::array<DictionaryOwnedText, kCapacity> items{};
  uint8_t count = 0;
};

struct DictionaryResult {
  DictionaryStatus status = DictionaryStatus::NotFound;
  DictionaryBackendKind backend = DictionaryBackendKind::StarDict;
  size_t matchedBytes = 0;
  DictionaryOwnedText surface;
  DictionaryOwnedText headword;
  DictionaryOwnedText reading;
  bool transformed = false;
  // StarDict keeps alternate-form history distinct from ordinary stemming.
  // Japanese deinflection leaves this false and uses transformed alone.
  bool alternate = false;
  bool syntheticName = false;
  uint8_t sourceMask = 0;
  DictionaryDefinitionHandle definition;
};

enum class DictionaryDefinitionMode : uint8_t { Styled, PlainFallback };

struct DictionaryDefinitionSpan {
  std::string_view text;
  bool bold = false;
  bool italic = false;
  bool superscript = false;
  bool subscript = false;
  bool ipa = false;
  bool underline = false;
  bool strikethrough = false;
  bool listItem = false;
  bool lineBreak = false;
  uint8_t indentLevel = 0;
};

struct DictionaryDefinitionSink {
  void* context = nullptr;
  bool (*onSpan)(void*, const DictionaryDefinitionSpan&) = nullptr;
};
