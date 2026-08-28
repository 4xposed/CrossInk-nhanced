#pragma once

#include <span>
#include <string>
#include <string_view>

namespace OpdsDownloadPath {

std::string normalizeConfiguredRoot(std::string_view configuredRoot);
std::string buildDestinationPath(std::string_view configuredRoot, std::span<const std::string> catalogHierarchy,
                                 std::string_view filename);

}  // namespace OpdsDownloadPath
