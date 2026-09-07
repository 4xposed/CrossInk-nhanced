#pragma once
#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
namespace bookmutation {
enum class JsonKind : uint8_t { Recent, State };
enum class PathField : uint8_t { Book, Cover, Resume, Favorite, Preferred };
enum class PathEdit : uint8_t { Keep, Replace, Remove, Error };
using RewritePath = PathEdit (*)(void*, PathField, const char* book, const char* value, char* out, size_t capacity);
// All scratch belongs to the transaction's <=4KiB workspace. No document-sized
// allocation; unknown keys/values stream verbatim. Nested unknown JSON is bounded.
struct JsonScratch {
  char book[1024], value[1024], replacement[1024];
  uint8_t input[256], output[256], probeName[256];
};
static_assert(sizeof(JsonScratch) <= 4096);
bool rewriteSharedJson(FsFile& source, FsFile& destination, JsonKind, RewritePath, void*, JsonScratch&);
}  // namespace bookmutation
