#include "OpdsDownloadPath.h"

#include "util/StringUtils.h"

namespace {

bool isCatalogLandingCategory(const std::string_view title) {
  return title == "Recently updated" || title == "All series (A-Z)" || title == "Recently added";
}

}  // namespace

namespace OpdsDownloadPath {

std::string normalizeConfiguredRoot(const std::string_view root) {
  std::string normalized(root);
  const size_t first = normalized.find_first_not_of(" \t");
  if (first == std::string::npos) return "";
  normalized.erase(0, first);

  const size_t last = normalized.find_last_not_of(" \t");
  normalized.erase(last + 1);
  if (normalized == "/") return "";
  if (normalized.front() != '/') normalized.insert(normalized.begin(), '/');
  while (normalized.size() > 1 && normalized.back() == '/') normalized.pop_back();
  return normalized;
}

std::string buildDestinationPath(const std::string_view configuredRoot,
                                 const std::span<const std::string> catalogHierarchy, const std::string_view filename) {
  // This path must live through the user-triggered downloader call; construct it once, never from the render loop.
  std::string destination = normalizeConfiguredRoot(configuredRoot);
  size_t capacity = destination.size() + filename.size() + 1;
  for (const auto& title : catalogHierarchy) {
    capacity += title.size() + 1;
  }
  destination.reserve(capacity);

  for (const auto& title : catalogHierarchy) {
    if (isCatalogLandingCategory(title)) continue;
    destination += '/';
    destination += StringUtils::sanitizeFilename(title);
  }
  destination += '/';
  destination += StringUtils::sanitizeFilename(std::string(filename));
  return destination;
}

}  // namespace OpdsDownloadPath
