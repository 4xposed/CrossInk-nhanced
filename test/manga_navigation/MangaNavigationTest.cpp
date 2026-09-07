#include <gtest/gtest.h>

#include "MangaNavigation.h"

namespace {

using manga::Entry;
using manga::Move;
using manga::PageAvailability;
using manga::Position;

PageAvailability page(const bool overview, const uint16_t panelCount, const std::initializer_list<uint16_t> crops) {
  PageAvailability result{};
  result.overview = overview;
  result.panelCount = panelCount;
  for (const uint16_t crop : crops) result.setCrop(crop);
  return result;
}

void expectPosition(const Position actual, const uint32_t pageNumber, const int16_t panel) {
  EXPECT_EQ(actual.page, pageNumber);
  EXPECT_EQ(actual.panel, panel);
}

void expectMove(const Move actual, const uint32_t pageNumber, const int16_t panel, const Entry entry,
                const bool changePage, const bool changed) {
  expectPosition(actual.position, pageNumber, panel);
  EXPECT_EQ(actual.entry, entry);
  EXPECT_EQ(actual.changePage, changePage);
  EXPECT_EQ(actual.changed, changed);
}

TEST(MangaNavigation, CropBitsAreBoundedToFormatPanelLimit) {
  PageAvailability availability{};
  availability.setCrop(0);
  availability.setCrop(254);
  availability.setCrop(255);
  availability.setCrop(300);

  EXPECT_TRUE(availability.hasCrop(0));
  EXPECT_TRUE(availability.hasCrop(254));
  EXPECT_FALSE(availability.hasCrop(255));
  EXPECT_FALSE(availability.hasCrop(300));
}

TEST(MangaNavigation, ResolveEntryUsesRequestedOverviewAndRealEdgeCrops) {
  const PageAvailability availability = page(true, 5, {1, 3});

  expectPosition(manga::resolveEntry(7, Entry::Overview, availability, false), 7, -1);
  expectPosition(manga::resolveEntry(7, Entry::FirstPanel, availability, false), 7, 1);
  expectPosition(manga::resolveEntry(7, Entry::LastPanel, availability, false), 7, 3);
}

TEST(MangaNavigation, ResolveEntryForcesPanelsWhenOverviewIsUnavailableOrPanelsOnly) {
  expectPosition(manga::resolveEntry(2, Entry::Overview, page(false, 4, {2}), false), 2, 2);
  expectPosition(manga::resolveEntry(2, Entry::Overview, page(true, 4, {2}), true), 2, 2);
}

TEST(MangaNavigation, ResolveEntryFallsBackToOverviewWhenNoCropExists) {
  expectPosition(manga::resolveEntry(4, Entry::FirstPanel, page(true, 3, {}), false), 4, -1);
  expectPosition(manga::resolveEntry(4, Entry::LastPanel, page(true, 0, {}), true), 4, -1);
}

TEST(MangaNavigation, NormalizeRepairsStaleResumePositions) {
  const PageAvailability availability = page(true, 5, {0, 3});

  expectPosition(manga::normalizePosition({6, 1}, availability, false), 6, -1);
  expectPosition(manga::normalizePosition({6, 9}, availability, true), 6, 0);
  expectPosition(manga::normalizePosition({6, -4}, availability, false), 6, -1);
}

TEST(MangaNavigation, NextSkipsMissingMiddleCrop) {
  const PageAvailability availability = page(true, 5, {0, 3});

  expectMove(manga::next({1, -1}, 3, availability, false), 1, 0, Entry::FirstPanel, false, true);
  expectMove(manga::next({1, 0}, 3, availability, false), 1, 3, Entry::FirstPanel, false, true);
}

TEST(MangaNavigation, NextPageEntryPreservesOverviewUnlessPanelsAreRequired) {
  expectMove(manga::next({1, 3}, 4, page(true, 4, {0, 3}), false), 2, -1, Entry::Overview, true, true);
  expectMove(manga::next({1, 3}, 4, page(true, 4, {0, 3}), true), 2, -1, Entry::FirstPanel, true, true);
  expectMove(manga::next({1, 3}, 4, page(false, 4, {0, 3}), false), 2, -1, Entry::FirstPanel, true, true);
}

TEST(MangaNavigation, NextFromPageWithoutPanelsAdvancesAndStopsAtBookEnd) {
  expectMove(manga::next({1, -1}, 3, page(true, 0, {}), false), 2, -1, Entry::Overview, true, true);
  expectMove(manga::next({2, -1}, 3, page(true, 0, {}), false), 2, -1, Entry::Overview, false, false);
}

TEST(MangaNavigation, PreviousMovesFromPanelsToOverviewOrEarlierRealCrop) {
  const PageAvailability availability = page(true, 6, {1, 4});

  expectMove(manga::previous({2, 4}, 5, availability, false), 2, 1, Entry::LastPanel, false, true);
  expectMove(manga::previous({2, 1}, 5, availability, false), 2, -1, Entry::Overview, false, true);
}

TEST(MangaNavigation, PreviousFromFirstPanelCrossesPageWhenOverviewCannotBeUsed) {
  expectMove(manga::previous({2, 1}, 5, page(true, 3, {1}), true), 1, -1, Entry::LastPanel, true, true);
  expectMove(manga::previous({2, 1}, 5, page(false, 3, {1}), false), 1, -1, Entry::LastPanel, true, true);
}

TEST(MangaNavigation, PreviousOverviewCrossesToOverviewAndStopsAtBookStart) {
  expectMove(manga::previous({2, -1}, 5, page(true, 0, {}), false), 1, -1, Entry::Overview, true, true);
  expectMove(manga::previous({0, -1}, 5, page(true, 0, {}), false), 0, -1, Entry::Overview, false, false);
}

TEST(MangaNavigation, ZeroPageBookDoesNotMove) {
  expectMove(manga::next({8, 2}, 0, page(false, 3, {2}), true), 8, 2, Entry::Overview, false, false);
  expectMove(manga::previous({8, 2}, 0, page(false, 3, {2}), true), 8, 2, Entry::Overview, false, false);
}

}  // namespace
