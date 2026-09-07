#pragma once
#include <string>
#include <string_view>
namespace FsHelpers {
inline bool hasJpgExtension(std::string_view s) { return s.ends_with(".jpg"); }
inline bool hasBmpExtension(std::string_view s) { return s.size() >= 4 && s.substr(s.size() - 4) == ".bmp"; }
inline bool hasPngExtension(std::string_view s) { return s.size() >= 4 && s.substr(s.size() - 4) == ".png"; }
}  // namespace FsHelpers
