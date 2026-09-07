#pragma once

namespace manga {

struct ImageViewport {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct ImageGeometry {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// Both orientations are captured under RenderLock; asymmetric bezel insets
// cannot be reconstructed by exchanging viewport width and height.
struct ImageViewports {
  ImageViewport base;
  ImageViewport rotated;
  int screenWidth = 0;
  int screenHeight = 0;
  int orientation = 0;
};
struct ImageLayout {
  ImageGeometry geometry;
  int screenWidth = 0;
  int screenHeight = 0;
  int orientation = 0;
};
bool buildImageLayout(int sourceWidth, int sourceHeight, const ImageViewports& views, bool rotateAllowed, bool bitmap,
                      ImageLayout& out);

// Fits an image inside a non-negative, signed-16-bit viewport. Source and
// viewport dimensions must also fit in a positive signed 16-bit value.
bool fitImage(int sourceWidth, int sourceHeight, const ImageViewport& viewport, bool allowUpscale, ImageGeometry& out);

// Matches the manga reader's aspect policy: rotate a non-square image only
// when its portrait/landscape direction differs from a non-square screen.
bool shouldRotateImage(int sourceWidth, int sourceHeight, int screenWidth, int screenHeight, bool allowed);

// Applies Matcha's counterclockwise quarter-turn to a valid renderer
// orientation in the range 0..3.
int rotatedImageOrientation(int orientation);

}  // namespace manga
