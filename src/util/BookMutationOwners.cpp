#include "BookMutationOwners.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <esp_task_wdt.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string_view>

#include "BookMutationJournal.h"
#include "BookMutationJsonAllocator.h"
#include "BookMutationStorage.h"
#include "BookmarkStore.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/boot_sleep/ImageFolderIndex.h"
extern ActivityManager activityManager;
namespace bookmutation {
bool quiesceOwners() { return activityManager.prepareForFolderMutation(); }
void serviceMutation() {
  esp_task_wdt_reset();
  yield();
}
uint64_t mutationId() { return (uint64_t(micros()) << 32) ^ uint64_t(millis()) ^ 0x43524f5353494e4bULL; }
bool reloadSharedOwners() { return RECENT_BOOKS.loadFromFile(true) && APP_STATE.loadFromFile(true); }
bool bookmarkPath(const char* book, bool legacy, char* out, size_t n) {
  return BookmarkStore::folderMutationPath(book, legacy, out, n);
}
bool validateSharedOwner(bool, const char* path) {
  BoundedJsonAllocator allocator;
  JsonDocument document(&allocator);
  return readBoundedOwnerJson(path, document);
}
bool removeFileMetadataChecked(const char* book, char* path, char* scratch, uint8_t* names) {
  const bool ok = removeBookFileMetadata(book, path, scratch, names);
  if (ok) ImageFolderIndex::invalidateForPath(book);
  return ok;
}

}  // namespace bookmutation
