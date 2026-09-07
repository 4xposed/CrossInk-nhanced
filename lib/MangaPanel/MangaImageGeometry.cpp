#include "MangaImageGeometry.h"

#include <cstdint>
#include <limits>

namespace manga {
namespace {

constexpr int kMaxDimension = std::numeric_limits<int16_t>::max();

bool isValidDimension(const int value) { return value > 0 && value <= kMaxDimension; }

int roundedScale(const int value, const int numerator, const int denominator) {
  const int64_t scaled = static_cast<int64_t>(value) * numerator;
  return static_cast<int>((scaled + denominator / 2) / denominator);
}

}  // namespace

bool fitImage(const int sourceWidth, const int sourceHeight, const ImageViewport& viewport, const bool allowUpscale,
              ImageGeometry& out) {
  out = {};
  if (!isValidDimension(sourceWidth) || !isValidDimension(sourceHeight) || !isValidDimension(viewport.width) ||
      !isValidDimension(viewport.height) || viewport.x < 0 || viewport.y < 0 || viewport.x > kMaxDimension ||
      viewport.y > kMaxDimension) {
    return false;
  }

  const int64_t viewportRight = static_cast<int64_t>(viewport.x) + viewport.width;
  const int64_t viewportBottom = static_cast<int64_t>(viewport.y) + viewport.height;
  if (viewportRight > kMaxDimension || viewportBottom > kMaxDimension) return false;

  int fittedWidth = sourceWidth;
  int fittedHeight = sourceHeight;
  if (allowUpscale || sourceWidth > viewport.width || sourceHeight > viewport.height) {
    const int64_t sourceAtViewportHeight = static_cast<int64_t>(sourceWidth) * viewport.height;
    const int64_t viewportAtSourceHeight = static_cast<int64_t>(viewport.width) * sourceHeight;
    if (sourceAtViewportHeight > viewportAtSourceHeight) {
      fittedWidth = viewport.width;
      fittedHeight = roundedScale(sourceHeight, viewport.width, sourceWidth);
    } else {
      fittedHeight = viewport.height;
      fittedWidth = roundedScale(sourceWidth, viewport.height, sourceHeight);
    }
  }

  if (fittedWidth < 1) fittedWidth = 1;
  if (fittedHeight < 1) fittedHeight = 1;
  if (fittedWidth > viewport.width) fittedWidth = viewport.width;
  if (fittedHeight > viewport.height) fittedHeight = viewport.height;

  out.x = viewport.x + (viewport.width - fittedWidth) / 2;
  out.y = viewport.y + (viewport.height - fittedHeight) / 2;
  out.width = fittedWidth;
  out.height = fittedHeight;
  return true;
}

bool shouldRotateImage(const int sourceWidth, const int sourceHeight, const int screenWidth, const int screenHeight,
                       const bool allowed) {
  if (!allowed || !isValidDimension(sourceWidth) || !isValidDimension(sourceHeight) || !isValidDimension(screenWidth) ||
      !isValidDimension(screenHeight) || sourceWidth == sourceHeight || screenWidth == screenHeight) {
    return false;
  }
  return (sourceWidth > sourceHeight) != (screenWidth > screenHeight);
}

bool buildImageLayout(int sourceWidth, int sourceHeight, const ImageViewports& views, bool rotateAllowed, bool bitmap,
                      ImageLayout& out) {
  const bool rotate =
      shouldRotateImage(sourceWidth, sourceHeight, views.screenWidth, views.screenHeight, rotateAllowed);
  out.orientation = rotate ? rotatedImageOrientation(views.orientation) : views.orientation;
  out.screenWidth = rotate ? views.screenHeight : views.screenWidth;
  out.screenHeight = rotate ? views.screenWidth : views.screenHeight;
  return fitImage(sourceWidth, sourceHeight, rotate ? views.rotated : views.base, !bitmap, out.geometry);
}

int rotatedImageOrientation(const int orientation) { return (orientation + 3) % 4; }

}  // namespace manga
