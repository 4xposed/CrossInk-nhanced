#pragma once
#include <string>

#include "BookMutationJson.h"
namespace bookmutation {
enum class Presence : uint8_t { Missing, File, Directory, Error };
struct Fingerprint {
  uint64_t bytes = 0;
  uint32_t crc = 0;
  bool operator==(const Fingerprint&) const = default;
};
bool normalizedPath(const char*);
bool within(const char* path, const char* root);
Presence probe(const char*, char* scratch, uint8_t* name);
bool fingerprint(const char*, Fingerprint&, uint8_t* buffer);
bool copyFile(const char*, const char*, Fingerprint&, uint8_t* buffer);
bool ownedRemove(const char*, char* scratch, uint8_t* name);
bool makeParent(const char*, char* scratch, uint8_t* name);
bool durableCacheName(const char*);
uint32_t bookPathHash(const char*);
bool mangaCachePath(const char*, char*, size_t);
bool clearMangaDisposableCache(const std::string& cache);
bool removeBookFileMetadata(const char* book, char* path, char* scratch, uint8_t* names);
}  // namespace bookmutation
