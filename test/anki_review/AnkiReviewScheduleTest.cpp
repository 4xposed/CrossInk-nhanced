#include <gtest/gtest.h>

#include "activities/anki/AnkiReviewSchedule.h"

namespace {

constexpr uint32_t kPostGradeReviewCount = 10;

TEST(AnkiReviewSchedule, GradesExistingCardRelativeToPostGradeReviewCount) {
  const ReviewState current{8, 5, ReviewKind::Review, 0};

  const ReviewState again = AnkiReviewSchedule::applyGrade(current, AnkiReviewGrade::Again, kPostGradeReviewCount);
  EXPECT_EQ(again.dueDay, kPostGradeReviewCount + 1);
  EXPECT_EQ(again.intervalDays, 5);
  EXPECT_EQ(again.kind, ReviewKind::Review);

  const ReviewState hard = AnkiReviewSchedule::applyGrade(current, AnkiReviewGrade::Hard, kPostGradeReviewCount);
  EXPECT_EQ(hard.dueDay, kPostGradeReviewCount + 6);
  EXPECT_EQ(hard.intervalDays, 6);
  EXPECT_EQ(hard.kind, ReviewKind::Review);

  const ReviewState good = AnkiReviewSchedule::applyGrade(current, AnkiReviewGrade::Good, kPostGradeReviewCount);
  EXPECT_EQ(good.dueDay, kPostGradeReviewCount + 10);
  EXPECT_EQ(good.intervalDays, 10);
  EXPECT_EQ(good.kind, ReviewKind::Review);

  const ReviewState easy = AnkiReviewSchedule::applyGrade(current, AnkiReviewGrade::Easy, kPostGradeReviewCount);
  EXPECT_EQ(easy.dueDay, kPostGradeReviewCount + 15);
  EXPECT_EQ(easy.intervalDays, 15);
  EXPECT_EQ(easy.kind, ReviewKind::Review);
}

TEST(AnkiReviewSchedule, GradesNewCardRelativeToPostGradeReviewCount) {
  const ReviewState current{0, 1, ReviewKind::New, 0};

  const ReviewState again = AnkiReviewSchedule::applyGrade(current, AnkiReviewGrade::Again, kPostGradeReviewCount);
  EXPECT_EQ(again.dueDay, kPostGradeReviewCount + 1);
  EXPECT_EQ(again.intervalDays, 1);
  EXPECT_EQ(again.kind, ReviewKind::New);

  const ReviewState hard = AnkiReviewSchedule::applyGrade(current, AnkiReviewGrade::Hard, kPostGradeReviewCount);
  EXPECT_EQ(hard.dueDay, kPostGradeReviewCount + 1);
  EXPECT_EQ(hard.intervalDays, 1);
  EXPECT_EQ(hard.kind, ReviewKind::Review);

  const ReviewState good = AnkiReviewSchedule::applyGrade(current, AnkiReviewGrade::Good, kPostGradeReviewCount);
  EXPECT_EQ(good.dueDay, kPostGradeReviewCount + 1);
  EXPECT_EQ(good.intervalDays, 1);
  EXPECT_EQ(good.kind, ReviewKind::Review);

  const ReviewState easy = AnkiReviewSchedule::applyGrade(current, AnkiReviewGrade::Easy, kPostGradeReviewCount);
  EXPECT_EQ(easy.dueDay, kPostGradeReviewCount + 4);
  EXPECT_EQ(easy.intervalDays, 4);
  EXPECT_EQ(easy.kind, ReviewKind::Review);
}

TEST(AnkiReviewSchedule, HardUsesCeilingForOneReviewInterval) {
  const ReviewState current{0, 1, ReviewKind::Learning, 0};

  const ReviewState hard = AnkiReviewSchedule::applyGrade(current, AnkiReviewGrade::Hard, kPostGradeReviewCount);

  EXPECT_EQ(hard.intervalDays, 2);
  EXPECT_EQ(hard.dueDay, kPostGradeReviewCount + 2);
  EXPECT_EQ(hard.kind, ReviewKind::Review);
}

TEST(AnkiReviewSchedule, HardRoundsFractionalIntervalUp) {
  const ReviewState current{0, 3, ReviewKind::Review, 0};

  const ReviewState hard = AnkiReviewSchedule::applyGrade(current, AnkiReviewGrade::Hard, kPostGradeReviewCount);

  EXPECT_EQ(hard.intervalDays, 4);
  EXPECT_EQ(hard.dueDay, kPostGradeReviewCount + 4);
}

}  // namespace
