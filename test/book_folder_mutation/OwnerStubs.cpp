#include <HalStorage.h>

#include "BookmarkStore.h"
#include "util/BookMutationOwners.h"
#include "util/BookMutationStorage.h"
namespace bookmutation {
bool quiesceOwners() { return true; }
void serviceMutation() {}
uint64_t mutationId() { return 0x1122334455667788ULL; }
bool reloadSharedOwners() { return !mutation_test::failReload; }
bool validateSharedOwner(bool, const char*) { return true; }
bool removeFileMetadataChecked(const char* book, char* path, char* scratch, uint8_t* names) {
  return removeBookFileMetadata(book, path, scratch, names);
}
bool bookmarkPath(const char* book, bool legacy, char* out, size_t n) {
  return BookmarkStore::folderMutationPath(book, legacy, out, n);
}
}  // namespace bookmutation
