#include <cassert>
#include <cstring>

#include "util/LibraryPaths.h"
int main() {
  assert(library::containsPath("/epubs/", "/epubs/sub/book.epub"));
  assert(!library::containsPath("/epubs", "/epubs-old/book.epub"));
  assert(library::containsPath("/", "/manga/series"));
  char path[32] = "unchanged";
  assert(!library::normalizeFolder("/epubs/../private", path, sizeof(path)));
  assert(!library::normalizeFolder("relative", path, sizeof(path)));
  assert(!library::normalizeFolder("/too/long", path, 4));
  assert(library::normalizeFolder("//epubs///", path, sizeof(path)));
  assert(strcmp(path, "/epubs") == 0);
  assert(library::normalizeFolder("/", path, sizeof(path)));
  assert(strcmp(path, "/") == 0);
  assert(!library::normalizeFolder(std::string_view("/a\0b", 4), path, sizeof(path)));
}
