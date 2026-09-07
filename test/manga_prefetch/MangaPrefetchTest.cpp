#include <MangaImageGeometry.h>
#include <MangaPendingInput.h>
#include <MangaPrefetchState.h>
#include <gtest/gtest.h>

TEST(MangaPrefetchState, OneSlotRemainsOwnedUntilCleanupAcknowledged) {
  manga::PrefetchState slot;
  ASSERT_TRUE(slot.post());
  EXPECT_FALSE(slot.post());
  ASSERT_TRUE(slot.begin());
  slot.cancel();
  EXPECT_TRUE(slot.cancelled());
  EXPECT_FALSE(slot.idle());
  EXPECT_FALSE(slot.consume());
  slot.finish();  // Producer calls this only after all files and scratch are released.
  EXPECT_TRUE(slot.consume());
  EXPECT_TRUE(slot.idle());
}
TEST(MangaPrefetchState, CancelledGenerationCannotBeReused) {
  manga::PrefetchState slot;
  ASSERT_TRUE(slot.post());
  slot.cancel();
  ASSERT_TRUE(slot.begin());
  EXPECT_TRUE(slot.cancelled());
  slot.finish();
  ASSERT_TRUE(slot.consume());
  ASSERT_TRUE(slot.post());
  ASSERT_TRUE(slot.begin());
  EXPECT_FALSE(slot.cancelled());
}
TEST(MangaPrefetchState, CompletionNeedsOwnerConsumption) {
  manga::PrefetchState slot;
  EXPECT_FALSE(slot.begin());
  EXPECT_FALSE(slot.consume());
  ASSERT_TRUE(slot.post());
  ASSERT_TRUE(slot.begin());
  slot.finish();
  EXPECT_FALSE(slot.post());
  EXPECT_TRUE(slot.consume());
  EXPECT_TRUE(slot.post());
}
TEST(MangaPrefetchGeometry, UsesRotatedAsymmetricViewportAndBmpUpscalePolicy) {
  manga::ImageViewports view{{7, 11, 460, 770}, {31, 13, 750, 450}, 480, 800, 0};
  manga::ImageLayout out;
  ASSERT_TRUE(manga::buildImageLayout(200, 100, view, true, true, out));
  EXPECT_EQ(out.orientation, 3);
  EXPECT_EQ(out.screenWidth, 800);
  EXPECT_EQ(out.geometry.x, 306);
  EXPECT_EQ(out.geometry.y, 188);
  EXPECT_EQ(out.geometry.width, 200);
  ASSERT_TRUE(manga::buildImageLayout(200, 100, view, true, false, out));
  EXPECT_EQ(out.geometry.width, 750);
  EXPECT_EQ(out.geometry.height, 375);
  EXPECT_EQ(out.geometry.x, 31);
  EXPECT_EQ(out.geometry.y, 50);
  ASSERT_TRUE(manga::buildImageLayout(200, 100, view, false, false, out));
  EXPECT_EQ(out.orientation, 0);
  EXPECT_EQ(out.geometry.width, 460);
}

TEST(MangaPendingInput, OpposingMovesMergeBeforeEitherBoundaryIsApplied) {
  for (int boundary : {0, 3}) {
    manga::PendingInput intent;
    int position = boundary;
    intent.move(boundary == 0);  // Retained while worker owns its source.
    intent.move(boundary != 0);  // Fresh on the cleanup-completion iteration.
    const int direction = intent.takeMove();
    position = std::clamp(position + direction, 0, 3);
    EXPECT_EQ(position, boundary);
    EXPECT_FALSE(intent.hasMove());
  }
}
TEST(MangaPendingInput, MenuRequestsCoalesceAndAreConsumedExactlyOnce) {
  manga::PendingInput intent;
  intent.requestMenu();
  intent.requestMenu();
  EXPECT_TRUE(intent.takeMenu());
  EXPECT_FALSE(intent.hasMenu());
  EXPECT_FALSE(intent.takeMenu());
}
TEST(MangaPendingInput, RotationsComposeBeforeSettingsAreWritten) {
  manga::PendingInput intent;
  intent.rotate(true);
  intent.rotate(false);
  EXPECT_EQ(intent.takeRotations(), 0);
  intent.rotate(false);
  EXPECT_EQ(intent.takeRotations(), 3);
  EXPECT_EQ(intent.takeRotations(), 0);
}
