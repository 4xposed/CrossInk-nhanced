#pragma once
#include <cstddef>
#include <cstdint>
namespace BookFolderMutation {
enum class Result : uint8_t {
  Complete,
  Busy,
  InvalidPath,
  SnapshotLimit,
  Collision,
  StorageError,
  MutationFailed,
  RecoveryPending,
  NotManga
};
// Already normalized absolute paths only. NotManga means a complete directory
// scan found no manga and no physical operation occurred; caller retains policy.
Result move(const char* oldRoot, const char* newRoot);
Result remove(const char* root, uint8_t maxDepth = 255);
Result removeMany(const char* const* roots, size_t count, uint8_t maxDepth = 255);
Result recoverPending();
Result retryPendingMutation();
bool hasPending();
bool storesFrozen();
const char* error(Result);
int httpStatus(Result, bool overwrite = true);
}  // namespace BookFolderMutation
