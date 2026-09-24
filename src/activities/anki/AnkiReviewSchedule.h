#pragma once

#include <AnkiDeckTypes.h>

#include <cstdint>
#include <limits>

// This is the device-local review policy; it intentionally does not emulate Anki scheduling.
enum class AnkiReviewGrade : uint8_t {
  Again,
  Hard,
  Good,
  Easy,
};

namespace AnkiReviewSchedule {

inline uint16_t clampInterval(const uint32_t interval) {
  return static_cast<uint16_t>(interval > std::numeric_limits<uint16_t>::max() ? std::numeric_limits<uint16_t>::max()
                                                                               : interval);
}

inline uint32_t addReviews(const uint32_t postGradeReviewCount, const uint16_t interval) {
  const uint64_t dueReviewCount = static_cast<uint64_t>(postGradeReviewCount) + interval;
  return dueReviewCount > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                               : static_cast<uint32_t>(dueReviewCount);
}

inline ReviewState applyGrade(const ReviewState& current, const AnkiReviewGrade grade,
                              const uint32_t postGradeReviewCount) {
  ReviewState next = current;
  if (grade == AnkiReviewGrade::Again) {
    next.dueDay = addReviews(postGradeReviewCount, 1);
    return next;
  }

  uint16_t interval = current.intervalDays == 0 ? 1 : current.intervalDays;
  if (current.kind == ReviewKind::New) {
    interval = grade == AnkiReviewGrade::Easy ? 4 : 1;
  } else {
    switch (grade) {
      case AnkiReviewGrade::Hard:
        interval = clampInterval((static_cast<uint32_t>(interval) * 6U + 4U) / 5U);
        break;
      case AnkiReviewGrade::Good:
        interval = clampInterval(static_cast<uint32_t>(interval) * 2U);
        break;
      case AnkiReviewGrade::Easy:
        interval = clampInterval(static_cast<uint32_t>(interval) * 3U);
        if (interval < 4) interval = 4;
        break;
      case AnkiReviewGrade::Again:
        break;
    }
  }

  next.intervalDays = interval;
  next.dueDay = addReviews(postGradeReviewCount, interval);
  next.kind = ReviewKind::Review;
  return next;
}

}  // namespace AnkiReviewSchedule
