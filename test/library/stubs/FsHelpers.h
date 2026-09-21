#pragma once
#include <algorithm>
#include <string>
namespace FsHelpers {
inline bool extension(std::string_view value, const char* ext) {
  std::string path(value);
  std::transform(path.begin(), path.end(), path.begin(), ::tolower);
  const std::string e(ext);
  return path.size() >= e.size() && path.substr(path.size() - e.size()) == e;
}
inline bool hasEpubExtension(std::string_view p) { return extension(p, ".epub"); }
inline bool hasTxtExtension(std::string_view p) { return extension(p, ".txt"); }
inline bool hasMarkdownExtension(std::string_view p) { return extension(p, ".md") || extension(p, ".markdown"); }
inline bool hasXtcExtension(std::string_view p) { return extension(p, ".xtc") || extension(p, ".xtch"); }
}  // namespace FsHelpers
