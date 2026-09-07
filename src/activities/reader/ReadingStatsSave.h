#pragma once
#include <string>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

struct ReadingStatsSaveResult {
  bool book = false;
  bool global = false;
  bool complete() const { return book && global; }
};
// Call with the same frozen snapshots/span. Each failed target gets one immediate
// retry; a successfully published day span must never be replayed.
ReadingStatsSaveResult saveReadingStatsWithRetry(const std::string& cachePath, const BookReadingStats& book,
                                                 const GlobalReadingStats& global,
                                                 const ReadingLanguageSpan* span = nullptr,
                                                 ReadingStatsSaveResult saved = {});

// Completion/date edits remain pending until both required files publish.
struct ReadingStatsEditState {
  bool dirty = false;
  bool failed = false;
  ReadingStatsSaveResult saved;
  void changed() {
    dirty = true;
    failed = false;
    saved = {};
  }
  bool persist(const std::string& cachePath, const BookReadingStats&, const GlobalReadingStats&);
};
