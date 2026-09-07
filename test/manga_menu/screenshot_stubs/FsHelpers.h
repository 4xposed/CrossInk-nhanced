#pragma once
#include <cstdio>
namespace FsHelpers {
inline void sanitizePathComponentForFat32(const char* in, char* out, size_t n) { std::snprintf(out, n, "%s", in); }
}  // namespace FsHelpers
