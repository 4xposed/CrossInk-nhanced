#pragma once

#include <cstddef>
#include <optional>

/** Tracks the catalog entry eligible to open immediately after its download. */
class OpdsDownloadedBook final {
 public:
  void record(const size_t entryIndex) { entryIndex_ = entryIndex; }

  [[nodiscard]] bool matches(const size_t entryIndex) const {
    return entryIndex_.has_value() && *entryIndex_ == entryIndex;
  }

  void clear() { entryIndex_.reset(); }

 private:
  std::optional<size_t> entryIndex_;
};
