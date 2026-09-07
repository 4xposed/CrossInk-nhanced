#pragma once

#include <Logging.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

enum class DictionaryScanIdentityStatus : uint8_t { Pending, Ready, Unavailable, ReadError, OutOfMemory, Cancelled };

// Activation-owned, handle-free progress. A Ready digest is a canonical content
// fingerprint, not cryptographic integrity. Active dictionary files are immutable.
class DictionaryScanIdentityState {
 public:
  DictionaryScanIdentityState() = default;
  DictionaryScanIdentityState(DictionaryScanIdentityState&& other) noexcept { *this = std::move(other); }
  DictionaryScanIdentityState& operator=(DictionaryScanIdentityState&& other) noexcept {
    if (this == &other) return *this;
    hash_ = other.hash_;
    for (unsigned i = 0; i < 4; ++i) sizes_[i] = other.sizes_[i];
    for (unsigned i = 0; i < 3; ++i) {
      dataSizes_[i] = other.dataSizes_[i];
      japanesePaths_[i] = other.japanesePaths_[i];
    }
    starPath_ = std::move(other.starPath_);
    offset_ = other.offset_;
    presentMask_ = other.presentMask_;
    backend_ = other.backend_;
    source_ = other.source_;
    status_ = other.status_;
    other.cancel();
    other.backend_ = 0;
    return *this;
  }
  DictionaryScanIdentityState(const DictionaryScanIdentityState&) = delete;
  DictionaryScanIdentityState& operator=(const DictionaryScanIdentityState&) = delete;
  DictionaryScanIdentityStatus status() const { return status_; }
  uint64_t digest() const { return status_ == DictionaryScanIdentityStatus::Ready ? hash_ : 0; }
  void cancel() {
    status_ = DictionaryScanIdentityStatus::Cancelled;
    hash_ = 0;
  }

 private:
  friend class DictIndex;
  friend class StarDictBackend;
  friend class DictionaryEngine;
  void bytes(const void* data, size_t length) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; ++i) {
      hash_ ^= p[i];
      hash_ *= UINT64_C(1099511628211);
    }
  }
  // Explicit little endian and length-delimited strings make host/target equal.
  void number(uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
      const uint8_t byte = value & 255;
      bytes(&byte, 1);
      value >>= 8;
    }
  }
  void text(const char* value, size_t length) {
    number(length);
    bytes(value, length);
  }
  void start(uint8_t backend) {
    *this = DictionaryScanIdentityState{};
    status_ = DictionaryScanIdentityStatus::Pending;
    backend_ = backend;
    constexpr char domain[] = "CrossInk canonical candidate scan v1";
    text(domain, sizeof(domain) - 1);
    number(backend);
  }
  DictionaryScanIdentityStatus fail(DictionaryScanIdentityStatus status) {
    LOG_ERR("DICT", "Scan identity disabled: status=%u source=%u offset=%u", static_cast<unsigned>(status),
            static_cast<unsigned>(source_), static_cast<unsigned>(offset_));
    status_ = status;
    hash_ = 0;
    return status_;
  }
  uint64_t hash_ = UINT64_C(14695981039346656037);
  uint64_t sizes_[4]{};
  uint64_t dataSizes_[3]{};
  const char* japanesePaths_[3]{};  // Only pointers into the immutable compiled path table.
  std::unique_ptr<char[]> starPath_;
  // HAL sizes and seek offsets are size_t on both host and firmware; captured
  // extents originate in that API, and each addition is bounded by size-offset.
  size_t offset_ = 0;
  uint8_t presentMask_ = 0;
  uint8_t backend_ = 0;
  uint8_t source_ = 0;
  DictionaryScanIdentityStatus status_ = DictionaryScanIdentityStatus::Unavailable;
};
