#pragma once

// The request hook is atomic-only. Send it BEFORE waiting for the rendering
// owner; the existing readiness hook still executes with its usual lock held.
template <typename Lock, typename ActivityType>
bool prepareBackgroundSuspension(ActivityType* activity) {
  if (activity) activity->requestBackgroundCancellation();
  Lock lock;
  return !activity || activity->prepareToSuspend();
}

// A failed persistence attempt must return to interactive input instead of the
// background-drain loop, which deliberately blocks input until its owner drains.
template <typename ActivityType>
bool retryBackgroundSuspensionAfterFailure(const ActivityType* activity) {
  return !activity || !activity->cancelSuspensionOnFailure();
}
