#include "MangaPrefetch.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <MangaBitmapPixels.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#ifdef SIMULATOR
#include <cstdlib>
#endif
#ifndef SIMULATOR
#include <esp_heap_caps.h>
#endif
#include "Epub/converters/JpegToFramebufferConverter.h"
#include "Epub/converters/PngToFramebufferConverter.h"

namespace manga {
#ifdef SIMULATOR
namespace {
std::atomic<bool> testHoldingSource{false}, testHoldConsumption{false}, testCompletionReady{false};
}
bool prefetchTestHoldingSource() { return testHoldingSource.load(std::memory_order_acquire); }
void prefetchTestHoldConsumption(bool hold) { testHoldConsumption.store(hold, std::memory_order_release); }
bool prefetchTestCompletionReady() { return testCompletionReady.load(std::memory_order_acquire); }
#endif
size_t jpegPrefetchDecoderBytes();
size_t pngPrefetchDecoderBytes();
namespace {
constexpr uint32_t kStackBytes = 8192;
constexpr size_t kInternalReserve = 32 * 1024;
constexpr size_t kBandBytes = 24 * 1024 + 512;
// FsFile, HAL path copies and allocator/task metadata. Codec internals are
// embedded in jpegPrefetchDecoderBytes()/pngPrefetchDecoderBytes(), not the old decoder estimates.
constexpr size_t kOverheadBytes = 4096;
bool admitted(size_t bytes, size_t largest) {
#ifdef SIMULATOR
  const size_t free = ESP.getFreeHeap(), block = ESP.getMaxAllocHeap();
#else
  const size_t free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
  if (free >= bytes + kInternalReserve && block >= largest + kOverheadBytes) return true;
  LOG_DBG("MANGA", "Prefetch memory skip: need=%u largest=%u free=%u block=%u", static_cast<unsigned>(bytes),
          static_cast<unsigned>(largest), static_cast<unsigned>(free), static_cast<unsigned>(block));
  return false;
}
}  // namespace
void applyImageLayout(const ImageLayout& layout, int sourceWidth, int sourceHeight, bool bitmap,
                      PixelIdentity& identity, RenderConfig& config) {
  const auto& g = layout.geometry;
  config.x = g.x;
  config.y = g.y;
  config.maxWidth = g.width;
  config.maxHeight = g.height;
  config.useExactDimensions = true;
  config.useGrayscale = true;
  config.useDithering = !bitmap;
  identity.sourceWidth = sourceWidth;
  identity.sourceHeight = sourceHeight;
  identity.width = g.width;
  identity.height = g.height;
  identity.x = g.x;
  identity.y = g.y;
  identity.screenWidth = layout.screenWidth;
  identity.screenHeight = layout.screenHeight;
  identity.orientation = layout.orientation;
  identity.flags = PixelPolicyGrayscale | (bitmap ? 0 : PixelPolicyDither);
}
MangaPrefetch::MangaPrefetch(const std::string& folder, size_t pathCapacity) : pathCapacity(pathCapacity) {
  // Two bounded, fallible lifetime path buffers; no string reserve/copy can
  // abort setup under -fno-exceptions. Overlong paths disable speculation.
  if (pathCapacity <= 1024 && folder.size() < pathCapacity) {
    this->folder = makeUniqueNoThrow<char[]>(folder.size() + 1);
    source = makeUniqueNoThrow<char[]>(pathCapacity);
    if (this->folder) memcpy(this->folder.get(), folder.c_str(), folder.size() + 1);
  }
  config.cancellation = {cancelled, this};
}
MangaPrefetch::~MangaPrefetch() { stopAndJoin(); }
bool MangaPrefetch::start() {
  if (!folder || !source) {
    LOG_ERR("MANGA", "Cannot allocate bounded prefetch paths");
    return false;
  }
  if (!admitted(kStackBytes + kOverheadBytes, kStackBytes)) return false;
  if (xTaskCreatePinnedToCore(trampoline, "MangaPrefetch", kStackBytes, this, 0, &task, 0) != pdPASS) {
    task = nullptr;
    LOG_ERR("MANGA", "Cannot create prefetch task; warming disabled");
    return false;
  }
  LOG_INF("MANGA", "Prefetch budget: JPEG=%u PNG=%u band=%u row=%u stack=%u scratch=%u reserve=%u",
          static_cast<unsigned>(jpegPrefetchDecoderBytes()), static_cast<unsigned>(pngPrefetchDecoderBytes()),
          static_cast<unsigned>(kBandBytes), 2048U, static_cast<unsigned>(kStackBytes),
          static_cast<unsigned>(kBitmapPixelScratchBytes), static_cast<unsigned>(kInternalReserve));
  return true;
}
bool MangaPrefetch::post(const char* path, uint32_t number, int16_t crop, bool allowRotate,
                         const ImageViewports& snapshot) {
  if (!task || !state.idle() || stopping.load(std::memory_order_acquire) || !path || strlen(path) >= pathCapacity)
    return false;
  memcpy(source.get(), path, strlen(path) + 1);
  page = number;
  panel = crop;
  rotate = allowRotate;
  views = snapshot;
#ifdef SIMULATOR
  testCompletionReady.store(false, std::memory_order_release);
#endif
  return state.post();
}
bool MangaPrefetch::cancelled(void* context) {
  auto& worker = *static_cast<MangaPrefetch*>(context);
  const bool cancelled = worker.stopping.load(std::memory_order_acquire) || worker.state.cancelled();
#ifdef SIMULATOR
  // The second fingerprint poll is checksumPayload's first poll with its
  // source file open. Hold exactly that boundary until foreground cancellation.
  if (!cancelled && worker.testingFingerprint && ++worker.testingFingerprintChecks == 2 &&
      std::getenv("CROSSINK_SIMULATOR_MANGA_PREFETCH_STRESS")) {
    testHoldingSource.store(true, std::memory_order_release);
    while (!worker.stopping.load(std::memory_order_acquire) && !worker.state.cancelled()) vTaskDelay(1);
    testHoldingSource.store(false, std::memory_order_release);
    return true;
  }
#endif
  return cancelled;
}
bool MangaPrefetch::poll(bool& failed) {
#ifdef SIMULATOR
  if (testHoldConsumption.load(std::memory_order_acquire)) return false;
#endif
  if (!state.consume()) return false;
  failed = result == Result::Failed;
  LOG_DBG("MANGA", "Prefetch result=%d page=%lu panel=%d", static_cast<int>(result), static_cast<unsigned long>(page),
          panel);
  return true;
}
void MangaPrefetch::stopAndJoin() {
  if (!task) return;
  stopping.store(true, std::memory_order_release);
  state.cancel();
  // Terminal teardown only. Worker never takes RenderLock or waits on owner.
  while (!stopped.load(std::memory_order_acquire)) vTaskDelay(1);
  task = nullptr;
}
void MangaPrefetch::trampoline(void* context) {
  auto* worker = static_cast<MangaPrefetch*>(context);
  while (!worker->stopping.load(std::memory_order_acquire)) {
    if (worker->state.begin()) {
      worker->result = worker->produce();
      if (worker->state.cancelled()) worker->result = Result::Cancelled;
      worker->cache.close();
      worker->cache.discardTemporary();
      // produce's decoder, files and allocation owners have all returned.
      worker->state.finish();
#ifdef SIMULATOR
      testCompletionReady.store(true, std::memory_order_release);
#endif
    }
    vTaskDelay(10);
  }
  worker->cache.close();
  worker->cache.discardTemporary();
#ifndef SIMULATOR
  LOG_INF("MANGA", "Prefetch stopped: stack high-water=%u internal-free=%u largest=%u",
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
#endif
  worker->stopped.store(true, std::memory_order_release);  // Last access to context.
  vTaskDelete(nullptr);
}
MangaPrefetch::Result MangaPrefetch::produce() {
  const auto cancellation = config.cancellation;
  if (cancellation.requested()) return Result::Cancelled;
  const bool bitmap = FsHelpers::hasBmpExtension(std::string_view(source.get()));
  const bool png = FsHelpers::hasPngExtension(std::string_view(source.get()));
  // The resident task stack is already deducted from current heap.
  const size_t codec = bitmap ? 0 : png ? pngPrefetchDecoderBytes() : jpegPrefetchDecoderBytes();
  const size_t extra =
      bitmap ? kBitmapPixelScratchBytes + kOverheadBytes : codec + kBandBytes + (png ? 2048 : 0) + kOverheadBytes;
  if (!admitted(extra, bitmap ? kBitmapPixelScratchBytes : std::max(codec, kBandBytes))) return Result::Failed;
  ImageDimensions dims{};
  JpegToFramebufferConverter jpeg;
  PngToFramebufferConverter pngDecoder;
  ImageToFramebufferDecoder* decoder = png ? static_cast<ImageToFramebufferDecoder*>(&pngDecoder) : &jpeg;
  if (bitmap) {
    BitmapPixelInfo info;
    if (!probeBitmapPixels(source.get(), info, cancellation)) return Result::Failed;
    if (info.bitsPerPixel == 1) return Result::Skipped;
    dims = {static_cast<int16_t>(info.width), static_cast<int16_t>(info.height)};
  } else if (!decoder->getDimensionsForCache(source.get(), dims)) {
    return Result::Failed;
  }
  if (cancellation.requested()) return Result::Cancelled;
  ImageLayout layout;
  if (!buildImageLayout(dims.width, dims.height, views, rotate, bitmap, layout)) return Result::Skipped;
  PixelIdentity identity;
#ifdef SIMULATOR
  testingFingerprint = true;
  testingFingerprintChecks = 0;
#endif
  const bool fingerprinted = fingerprintImage(source.get(), identity, cancellation);
#ifdef SIMULATOR
  testingFingerprint = false;
#endif
  if (!fingerprinted) return Result::Failed;
  applyImageLayout(layout, dims.width, dims.height, bitmap, identity, config.render);
  config.screenWidth = layout.screenWidth;
  config.screenHeight = layout.screenHeight;
  if (!cache.configure(folder.get(), page, panel, identity, cancellation)) return Result::Failed;
  if (cache.open(cancellation)) return Result::Hit;
  if (cancellation.requested()) return Result::Cancelled;
  config.render.cachePathOverride = cache.temporaryPath();
  // One fallible allocation per BMP job, never per row; release before the
  // ownership acknowledgment so idle warming does not tax foreground PNGs.
  auto scratch = bitmap ? makeUniqueNoThrow<uint8_t[]>(kBitmapPixelScratchBytes) : nullptr;
  if (bitmap && !scratch) {
    LOG_ERR("MANGA", "Cannot allocate prefetch BMP rows");
    return Result::Failed;
  }
  const bool produced =
      bitmap ? writeBitmapPixels(source.get(), cache.temporaryPath(), layout.geometry.width, layout.geometry.height,
                                 scratch.get(), kBitmapPixelScratchBytes, cancellation)
             : decoder->decodeToCache(source.get(), config);
  if (cancellation.requested()) return Result::Cancelled;
  if (!produced || !cache.publish(cancellation)) return Result::Failed;
  return Result::Warmed;
}
}  // namespace manga
