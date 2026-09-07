#pragma once
#include <cstddef>
#include <cstdint>
namespace bookmutation {
// App-lifecycle/store boundaries. Main-task only; no renderer work in the core.
bool quiesceOwners();
void serviceMutation();
uint64_t mutationId();
bool reloadSharedOwners();
bool validateSharedOwner(bool recent, const char* backup);
bool removeFileMetadataChecked(const char* book, char* path, char* scratch, uint8_t* names);
bool bookmarkPath(const char* book, bool legacy, char* out, size_t capacity);
class OwnerFreeze;
}  // namespace bookmutation
