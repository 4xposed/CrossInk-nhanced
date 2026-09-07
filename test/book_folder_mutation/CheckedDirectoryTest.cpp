#include <CheckedDirectoryEof.h>
#include <HalStorage.h>
#include <gtest/gtest.h>
using namespace checked_directory;
TEST(CheckedDirectory, FatPositiveMarkerAndPhysicalLastSlot) {
  EXPECT_TRUE(clean(plan(32, 64, false), 32, 0, true, false));
  EXPECT_TRUE(clean(plan(512, 512, false), 0, 0, true, false));
  EXPECT_FALSE(clean(plan(32, 64, false), 32, 0xe5, true, false));  // Deleted final slot, no marker.
  EXPECT_FALSE(clean(plan(32, 64, false), 0, 0, true, false));      // Full directory after skipped entries.
  EXPECT_FALSE(clean(plan(32, 64, false), 32, 'B', true, false));   // LFN checksum abort, no error bit.
  EXPECT_FALSE(clean(plan(32, 64, false), 31, 0, true, false));
}
TEST(CheckedDirectory, ParentCardSeekErrorsAndUnalignedPositionsReject) {
  EXPECT_EQ(plan(0, 32, true), Probe::Reject);
  EXPECT_EQ(plan(0, 31, false), Probe::Reject);
  EXPECT_EQ(plan(32, 0, false), Probe::Reject);
  EXPECT_FALSE(clean(Probe::Current, -1, 0, true, true));
  EXPECT_FALSE(clean(Probe::LastEntry, 32, 0, false, false));
  EXPECT_FALSE(clean(Probe::LastEntry, 32, 0, true, true));
}
TEST(CheckedDirectory, InvalidParentLatchAndCloseReset) {
  HalFile invalid;
  EXPECT_FALSE(invalid.openNextFileChecked());
  EXPECT_TRUE(invalid.enumerationFailed());
  EXPECT_TRUE(invalid.close());
  EXPECT_FALSE(invalid.enumerationFailed());
}
