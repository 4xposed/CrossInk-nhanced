#include <gtest/gtest.h>

#include "ReaderTouchLookupHold.h"

TEST(ReaderTouchLookupHold, CapturesAtHalfSecondButNotBefore) {
  ReaderTouchLookupHold hold;
  EXPECT_FALSE(hold.capture(true, 100, 200, 499));
  EXPECT_FALSE(hold.pending());
  EXPECT_TRUE(hold.capture(true, 100, 200, 500));
  EXPECT_TRUE(hold.pending());
}

TEST(ReaderTouchLookupHold, KeepsOriginalCoordinatesWhileRenderIsBusyAndFingerReleases) {
  ReaderTouchLookupHold hold;
  ASSERT_TRUE(hold.capture(true, 100, 200, 600));
  EXPECT_FALSE(hold.capture(true, 120, 220, 800));
  EXPECT_FALSE(hold.capture(false, 0, 0, 0));
  ASSERT_TRUE(hold.pending());
  EXPECT_EQ(hold.x(), 100);
  EXPECT_EQ(hold.y(), 200);
  hold.consume();
  EXPECT_FALSE(hold.pending());
}

TEST(ReaderTouchLookupHold, DoesNotPromoteShortTapSwipeOrCancelledContact) {
  ReaderTouchLookupHold hold;
  EXPECT_FALSE(hold.capture(true, 100, 200, 300));
  EXPECT_FALSE(hold.capture(false, 120, 220, 600));
  EXPECT_FALSE(hold.pending());
}

TEST(ReaderTouchLookupHold, OneLookupPerContactAndNextContactWorks) {
  ReaderTouchLookupHold hold;
  ASSERT_TRUE(hold.capture(true, 100, 200, 500));
  hold.consume();
  EXPECT_FALSE(hold.capture(true, 100, 200, 900));
  EXPECT_FALSE(hold.pending());
  hold.capture(false, 0, 0, 0);
  EXPECT_TRUE(hold.capture(true, 50, 60, 500));
  EXPECT_EQ(hold.x(), 50);
  EXPECT_EQ(hold.y(), 60);
}

TEST(ReaderTouchLookupHold, ResetDiscardsQueuedLookupForOldPage) {
  ReaderTouchLookupHold hold;
  ASSERT_TRUE(hold.capture(true, 100, 200, 600));
  hold.reset();
  EXPECT_FALSE(hold.pending());
  EXPECT_TRUE(hold.capture(true, 50, 60, 500));
}
