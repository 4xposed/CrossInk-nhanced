#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Per-book furigana glossary harvested from EPUB <ruby> markup.
// firmware dictionary lookup uses lookupOwned() so every owned result allocation is fallible and
// can be transferred without another text copy.
namespace RubyGlossary {

using Pair = std::pair<std::string, std::string>;

constexpr uint8_t kFileVersion = 1;
constexpr size_t kMaxTextBytes = 32;
constexpr size_t kMaxPairsPerSection = 200;
constexpr uint16_t kMaxFileRecords = 1024;
constexpr size_t kMaxFileBytes = 16 * 1024;

enum class LookupStatus : uint8_t { Found, NotFound, OutOfMemory };

struct HarvestCompletion {
  bool previewBuild = false;
  bool previewStopped = false;
  bool malformedMarkupTruncated = false;

  constexpr bool canMerge() const { return !previewBuild && !previewStopped && !malformedMarkupTruncated; }
};

class OwnedReadings {
 public:
  OwnedReadings() = default;
  OwnedReadings(const OwnedReadings&) = delete;
  OwnedReadings& operator=(const OwnedReadings&) = delete;
  OwnedReadings(OwnedReadings&&) noexcept = default;
  OwnedReadings& operator=(OwnedReadings&&) noexcept = default;

  std::string_view view() const { return storage_ ? std::string_view(storage_.get(), length_) : std::string_view{}; }
  size_t length() const { return length_; }
  std::unique_ptr<char[]> releaseStorage() { return std::move(storage_); }
  void reset() {
    storage_.reset();
    length_ = 0;
  }

 private:
  friend LookupStatus lookupOwned(std::string_view, std::string_view, OwnedReadings&);
  std::unique_ptr<char[]> storage_;
  size_t length_ = 0;
};

void collect(std::vector<Pair>& pairs, const std::string& base, const std::string& ruby);
void collectView(std::vector<Pair>& pairs, std::string_view base, std::string_view ruby);

void resetElement(std::string& elementBase, std::string& elementRuby, int& runCount);
void resetHarvest(std::vector<Pair>& pairs, std::string& elementBase, std::string& elementRuby, int& runCount);
void collectRun(std::vector<Pair>& pairs, std::string& elementBase, std::string& elementRuby, int& runCount,
                std::string_view base, std::string_view ruby);
void finishElement(std::vector<Pair>& pairs, std::string& elementBase, std::string& elementRuby, int& runCount);

// Best-effort atomic merge into <bookCachePath>/ruby.bin.
void merge(const std::string& bookCachePath, const std::vector<Pair>& pairs);

// follows the legacy API's allocator contract. New firmware code must prefer lookupOwned().
bool lookup(const std::string& bookCachePath, const std::string& base, std::string& outReadings);

LookupStatus lookupOwned(std::string_view bookCachePath, std::string_view base, OwnedReadings& outReadings);

}  // namespace RubyGlossary
