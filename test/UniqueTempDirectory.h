#pragma once

#include <filesystem>
#include <random>
#include <string>

// CTest runs discovered cases in separate processes. Reserve each directory
// atomically so parallel cases and concurrent test runs never share fixtures.
inline std::filesystem::path uniqueTempDirectory(const char* prefix) {
  std::random_device random;
  for (;;) {
    auto path = std::filesystem::temp_directory_path() /
                (std::string(prefix) + "-" + std::to_string(random()) + "-" + std::to_string(random()));
    if (std::filesystem::create_directory(path)) return path;
  }
}
