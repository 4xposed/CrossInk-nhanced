#include "AnkiReviewCandidates.h"

void AnkiReviewCandidates::reset(const uint32_t reviewCount) {
  reviewCount_ = reviewCount;
  reviewCandidateCount_ = 0;
  newCandidateCount_ = 0;
}

void AnkiReviewCandidates::add(const uint32_t cardIndex, const ReviewState& state) {
  switch (state.kind) {
    case ReviewKind::New:
      if (newCandidateCount_ < kMaxNewCandidates) {
        newIndices_[newCandidateCount_++] = cardIndex;
      }
      return;
    case ReviewKind::Review:
    case ReviewKind::Learning:
      break;
  }

  if (state.dueDay > reviewCount_ || reviewCandidateCount_ == kMaxReviewCandidates) return;

  uint32_t insertAt = 0;
  while (insertAt < reviewCandidateCount_ && reviewDueCounts_[insertAt] <= state.dueDay) {
    ++insertAt;
  }
  for (uint32_t index = reviewCandidateCount_; index > insertAt; --index) {
    reviewIndices_[index] = reviewIndices_[index - 1];
    reviewDueCounts_[index] = reviewDueCounts_[index - 1];
  }
  reviewIndices_[insertAt] = cardIndex;
  reviewDueCounts_[insertAt] = state.dueDay;
  ++reviewCandidateCount_;
}

uint32_t AnkiReviewCandidates::count() const { return reviewCandidateCount_ + newCandidateCount_; }

uint32_t AnkiReviewCandidates::currentIndex() const {
  if (reviewCandidateCount_ != 0) return reviewIndices_[0];
  return newCandidateCount_ != 0 ? newIndices_[0] : 0;
}
