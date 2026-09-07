#pragma once
#include <ArduinoJson.h>

#include <cstddef>
namespace bookmutation {
// Cold owner deserialization, separate from the <=4KiB streaming workspace.
// Reject growth beyond32KiB instead of allowing unbounded JSON allocations.
class BoundedJsonAllocator final : public ArduinoJson::Allocator {
  size_t used = 0;

 public:
  static constexpr size_t Limit = 32768;
  void* allocate(size_t size) override;
  void deallocate(void* pointer) override;
  void* reallocate(void* pointer, size_t size) override;
};
bool readBoundedOwnerJson(const char* path, JsonDocument& document);
}  // namespace bookmutation
