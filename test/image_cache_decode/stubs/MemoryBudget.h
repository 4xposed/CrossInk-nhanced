#pragma once
#include <Memory.h>

#include <cstddef>
#include <cstdint>

namespace MemoryBudget {
// Mirrors lib/MemoryBudget: the converter static_asserts JPEGDEC fits this budget.
constexpr uint32_t JPEG_DECODER_APPROX_BYTES = 20U * 1024U;

inline bool hasHeapForImageDecoder(const char*, const char*, unsigned) { return true; }
// Host tests have no PSRAM, so decoders always use the internal heap.
inline MemoryPool jpegDecoderPool(size_t) { return MemoryPool::Internal; }
inline bool canUseInternalHeapForJpegDecoder(const ByteHeapSnapshot&) { return true; }
inline bool hasHeapForJpegDecoder(const char*, size_t, const char* = nullptr) { return true; }
}  // namespace MemoryBudget
