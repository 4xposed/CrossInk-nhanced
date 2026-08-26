#pragma once

#include <AnkiDeckTypes.h>

#include <array>
#include <cstdint>

class AnkiReviewCandidates final {
 public:
  static constexpr uint32_t kMaxReviewCandidates = 80;
  static constexpr uint32_t kMaxNewCandidates = 20;

  void reset(uint32_t reviewCount);
  void add(uint32_t cardIndex, const ReviewState& state);
  uint32_t count() const;
  uint32_t currentIndex() const;

 private:
  std::array<uint32_t, kMaxReviewCandidates> reviewIndices_{};
  std::array<uint32_t, kMaxReviewCandidates> reviewDueCounts_{};
  std::array<uint32_t, kMaxNewCandidates> newIndices_{};
  uint32_t reviewCount_ = 0;
  uint32_t reviewCandidateCount_ = 0;
  uint32_t newCandidateCount_ = 0;
};
