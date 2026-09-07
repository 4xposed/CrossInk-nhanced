#pragma once
#include <MangaImageGeometry.h>
#include <MangaPixelCache.h>
#include <MangaPrefetchState.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>
#include <string>

#include "Epub/converters/ImageToFramebufferDecoder.h"

namespace manga {
#ifdef SIMULATOR
bool prefetchTestHoldingSource();
void prefetchTestHoldConsumption(bool hold);
bool prefetchTestCompletionReady();
#endif
void applyImageLayout(const ImageLayout& layout, int sourceWidth, int sourceHeight, bool bitmap,
                      PixelIdentity& identity, RenderConfig& config);
class MangaPrefetch {
 public:
  explicit MangaPrefetch(const std::string& folder, size_t pathCapacity);
  ~MangaPrefetch();
  bool start();
  bool idle() const { return state.idle(); }
  bool post(const char* source, uint32_t page, int16_t panel, bool rotate, const ImageViewports& views);
  void cancel() { state.cancel(); }
  bool poll(bool& failed);
  void stopAndJoin();

 private:
  enum class Result : uint8_t { Warmed, Hit, Skipped, Failed, Cancelled };
  PrefetchState state;
  std::atomic<bool> stopping{false}, stopped{false};
  TaskHandle_t task = nullptr;
  std::unique_ptr<char[]> folder, source;
  size_t pathCapacity;
  uint32_t page = 0;
  int16_t panel = -1;
  bool rotate = false;
  ImageViewports views;
  CacheDecodeConfig config;
  MangaPixelCache cache;
  Result result = Result::Skipped;
#ifdef SIMULATOR
  bool testingFingerprint = false;
  unsigned testingFingerprintChecks = 0;
#endif
  static void trampoline(void* context);
  static bool cancelled(void* context);
  Result produce();
};
}  // namespace manga
