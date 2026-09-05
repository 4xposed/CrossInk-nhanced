#pragma once

class RenderLock {
 public:
  RenderLock() { held_ = true; }
  ~RenderLock() { held_ = false; }
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  static bool peek() { return held_; }

 private:
  static inline bool held_ = false;
};
