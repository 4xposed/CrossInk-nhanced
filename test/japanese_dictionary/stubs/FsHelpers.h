#pragma once

#include <strings.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "HalStorage.h"

namespace FsHelpers {
inline bool resolveRootDirectoryIgnoreCase(const char* expectedPath, char* resolvedPath,
                                           const size_t resolvedPathSize) {
  if (!expectedPath || expectedPath[0] != '/' || expectedPath[1] == '\0' || std::strchr(expectedPath + 1, '/') ||
      !resolvedPath || resolvedPathSize == 0) {
    return false;
  }

  const char* expectedName = expectedPath + 1;
  for (const auto& entry : std::filesystem::directory_iterator(hal_storage_test::root)) {
    const std::string name = entry.path().filename().string();
    if (!entry.is_directory() || strcasecmp(name.c_str(), expectedName) != 0) continue;
    const int written = std::snprintf(resolvedPath, resolvedPathSize, "/%s", name.c_str());
    return written > 0 && static_cast<size_t>(written) < resolvedPathSize;
  }
  return false;
}
}  // namespace FsHelpers
