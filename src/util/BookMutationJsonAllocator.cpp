#include "BookMutationJsonAllocator.h"

#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

#include <cstddef>
#include <cstdlib>
namespace bookmutation {
namespace {
struct alignas(std::max_align_t) Allocation {
  size_t size;
};
}  // namespace
void* BoundedJsonAllocator::allocate(size_t size) { return reallocate(nullptr, size); }
void BoundedJsonAllocator::deallocate(void* pointer) {
  if (!pointer) return;
  auto* header = static_cast<Allocation*>(pointer) - 1;
  used -= header->size;
  free(header);
}
void* BoundedJsonAllocator::reallocate(void* pointer, size_t size) {
  auto* old = pointer ? static_cast<Allocation*>(pointer) - 1 : nullptr;
  const size_t previous = old ? old->size : 0;
  if (size > Limit || used - previous > Limit - size) return nullptr;
  auto* next = static_cast<Allocation*>(realloc(old, sizeof(Allocation) + size));
  if (!next) return nullptr;
  next->size = size;
  used = used - previous + size;
  return next + 1;
}
bool readBoundedOwnerJson(const char* path, JsonDocument& document) {
  FsFile file = Storage.open(path);
  if (!file) return false;
  const bool bounded = file.fileSize64() <= 65536;
  const bool closed = file.close();
  if (!bounded || !closed) {
    LOG_ERR("MUTATE", "Shared owner snapshot too large/unreadable");
    return false;
  }
  return PersistableStoreBase::readDocFromFile(path, document) && !document.overflowed();
}
}  // namespace bookmutation
