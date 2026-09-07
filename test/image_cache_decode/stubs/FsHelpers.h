#pragma once
#include <string>
namespace FsHelpers {
inline bool hasJpgExtension(const std::string& s) { return s.ends_with(".jpg"); }
inline bool hasPngExtension(const std::string& s) { return s.ends_with(".png"); }
}  // namespace FsHelpers
