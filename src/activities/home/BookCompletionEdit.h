#pragma once
#include <string>

#include "activities/reader/ReadingStatsSave.h"

namespace BookActions {
// Owned by the completion activity, never a global: no reader or other edit can
// run while this frozen transaction is pending. The activity is fallibly heap
// allocated so its two summaries and metadata do not consume the C3 task stack.
struct CompletionEdit {
  std::string cachePath, title, author, thumbPath;
  BookReadingStats book;
  GlobalReadingStats global;
  ReadingStatsEditState persistence;
  bool initialized = false;
  bool sideEffectsDone = false;
};
}  // namespace BookActions
