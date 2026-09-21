#pragma once
#include <HalStorage.h>
namespace manga {
struct MangaBook {
  static bool isMangaFolder(const char* p) { return Storage.exists((std::string(p) + "/book.mki").c_str()); }
};
}  // namespace manga
