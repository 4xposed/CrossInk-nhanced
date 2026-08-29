#include <gtest/gtest.h>

#include "src/activities/browser/OpdsDownloadedBook.h"

TEST(OpdsDownloadedBook, OpensOnlyTheEntryThatJustFinishedDownloading) {
  OpdsDownloadedBook downloaded;
  downloaded.record(2);

  EXPECT_FALSE(downloaded.matches(1));
  EXPECT_TRUE(downloaded.matches(2));
  EXPECT_FALSE(downloaded.matches(1));
}
TEST(OpdsDownloadedBook, KeepsOpenAvailableWhenReturningToTheDownloadedEntry) {
  OpdsDownloadedBook downloaded;
  downloaded.record(2);

  EXPECT_FALSE(downloaded.matches(1));
  EXPECT_TRUE(downloaded.matches(2));
}

TEST(OpdsDownloadedBook, ClearsTheOpenActionWhenTheCatalogChanges) {
  OpdsDownloadedBook downloaded;
  downloaded.record(2);
  downloaded.clear();

  EXPECT_FALSE(downloaded.matches(2));
}
