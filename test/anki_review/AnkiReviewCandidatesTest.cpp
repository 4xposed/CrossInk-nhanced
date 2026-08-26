#include <gtest/gtest.h>

#include "activities/anki/AnkiReviewCandidates.h"

namespace {

TEST(AnkiReviewCandidates, OrdersDueReviewsBeforeNewCardsWithStableDueOrder) {
  AnkiReviewCandidates candidates;
  candidates.reset(/*reviewCount=*/4);
  candidates.add(/*cardIndex=*/4, ReviewState{3, 1, ReviewKind::Review, 0});
  candidates.add(/*cardIndex=*/2, ReviewState{2, 1, ReviewKind::Review, 0});
  candidates.add(/*cardIndex=*/3, ReviewState{2, 1, ReviewKind::Review, 0});
  candidates.add(/*cardIndex=*/5, ReviewState{0, 1, ReviewKind::New, 0});
  candidates.add(/*cardIndex=*/6, ReviewState{5, 1, ReviewKind::Review, 0});

  EXPECT_EQ(candidates.count(), 4U);
  EXPECT_EQ(candidates.currentIndex(), 2U);
}

TEST(AnkiReviewCandidates, CapsDueReviewsAndNewCardsIndependently) {
  AnkiReviewCandidates candidates;
  candidates.reset(/*reviewCount=*/0);
  for (uint32_t index = 0; index < 81; ++index) {
    candidates.add(index, ReviewState{0, 1, ReviewKind::Review, 0});
  }
  for (uint32_t index = 100; index < 121; ++index) {
    candidates.add(index, ReviewState{0, 1, ReviewKind::New, 0});
  }

  EXPECT_EQ(candidates.count(), 100U);
  EXPECT_EQ(candidates.currentIndex(), 0U);

  candidates.reset(/*reviewCount=*/0);
  for (uint32_t index = 100; index < 121; ++index) {
    candidates.add(index, ReviewState{0, 1, ReviewKind::New, 0});
  }
  EXPECT_EQ(candidates.count(), 20U);
  EXPECT_EQ(candidates.currentIndex(), 100U);
}

}  // namespace
