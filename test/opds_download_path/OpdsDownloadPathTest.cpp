#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "src/activities/browser/OpdsDownloadPath.h"

TEST(OpdsDownloadPath, CreatesNestedPathBelowConfiguredServerRoot) {
  const std::vector<std::string> catalogHierarchy{"Dragon Ball"};

  EXPECT_EQ(OpdsDownloadPath::buildDestinationPath("manga", catalogHierarchy, "Chapter 1.epub"),
            "/manga/Dragon Ball/Chapter 1.epub");
}

TEST(OpdsDownloadPath, NormalizesRootAndSanitizesCatalogSegments) {
  const std::vector<std::string> catalogHierarchy{"Dragon/Ball", "  .Arc?  "};

  EXPECT_EQ(OpdsDownloadPath::buildDestinationPath("  manga/  ", catalogHierarchy, "Chapter 1.epub"),
            "/manga/Dragon_Ball/Arc_/Chapter 1.epub");
}

TEST(OpdsDownloadPath, UsesCatalogHierarchyBelowSdRootWhenServerFolderIsEmpty) {
  const std::vector<std::string> catalogHierarchy{"Dragon Ball"};

  EXPECT_EQ(OpdsDownloadPath::buildDestinationPath("", catalogHierarchy, "Chapter 1.epub"),
            "/Dragon Ball/Chapter 1.epub");
}

TEST(OpdsDownloadPath, SkipsCatalogLandingCategories) {
  const std::vector<std::string> catalogHierarchy{"Recently updated", "All series (A-Z)", "Recently added",
                                                  "Dragon Ball"};

  EXPECT_EQ(OpdsDownloadPath::buildDestinationPath("manga", catalogHierarchy, "Chapter 1.epub"),
            "/manga/Dragon Ball/Chapter 1.epub");
}
