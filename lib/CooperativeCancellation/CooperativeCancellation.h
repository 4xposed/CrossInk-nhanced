#pragma once

// Borrowed callback/context; the caller keeps both alive until the operation
// returns. Poll only at bounded row/block/chunk boundaries, never per pixel.
struct CooperativeCancellation {
  bool (*callback)(void*) = nullptr;
  void* context = nullptr;

  bool requested() const { return callback && callback(context); }
};
