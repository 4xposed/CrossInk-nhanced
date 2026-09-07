#pragma once

#include <BmpConversionDimensions.h>
#include <CooperativeCancellation.h>

#include <cstdint>
#include <string>

namespace manga {
class MangaBook;
enum class ThumbnailResult { Cached, Published, Cancelled, Failed };
enum class ThumbnailStage { Validation, Crc, Conversion, Sync, Identity, Publication, Complete };
struct ThumbnailDiagnostics {
  ThumbnailResult result = ThumbnailResult::Failed;
  ThumbnailStage stage = ThumbnailStage::Validation;
  uint64_t sourceSize = 0;
  uint32_t sourceCrc = 0;
  int width = 0, height = 0;
  BmpConversionDimensions sourceDimensions;
  const char* sourceType = "unknown";
};
ThumbnailResult generateThumbnailControlled(MangaBook& book, const std::string& folder, int width, int height,
                                            CooperativeCancellation cancellation = {},
                                            ThumbnailDiagnostics* diagnostics = nullptr);

std::string cachePath(const std::string& folder);
std::string thumbnailTemplatePath(const std::string& folder);
std::string thumbnailPath(const std::string& folder, int width, int height);
bool generateThumbnail(MangaBook& book, const std::string& folder, int width, int height, bool* regenerated = nullptr);
bool fitThumbnailDimensions(int sourceWidth, int sourceHeight, int maxWidth, int maxHeight, int& width, int& height);
}  // namespace manga
