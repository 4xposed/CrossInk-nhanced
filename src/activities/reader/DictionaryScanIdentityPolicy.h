#pragma once
#include <DictionaryScanIdentity.h>

// Shared by the activity's work gate and skipLoopDelay: ineligible pending work
// must not introduce busy spinning. Input is polled before each loop chunk.
class DictionaryScanIdentityPolicy {
 public:
  static constexpr unsigned kInitialSteps = 16;
  static constexpr size_t kChunkBytes = 256;
  static constexpr uint32_t kInitialMs = 5;
  bool initialOpportunity() const { return initial_; }
  bool canLoad() const { return !bypassed_; }
  void initialFinished() { initial_ = false; }
  void progressiveStarted() {
    initial_ = false;
    bypassed_ = true;
  }
  bool eligible(DictionaryScanIdentityStatus status, bool engineOpen, bool exiting, bool flowWorker, bool actualWorker,
                bool definitionSettled, bool pendingTouch, bool completeWithoutSelection) const {
    return status == DictionaryScanIdentityStatus::Pending && engineOpen && !exiting && !flowWorker && !actualWorker &&
           (initial_ || (!pendingTouch && (definitionSettled || completeWithoutSelection)));
  }

 private:
  bool initial_ = true;
  bool bypassed_ = false;
};
