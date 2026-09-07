#include <GfxRenderer/Bitmap.h>
#include <GfxRenderer/BitmapHelpers.h>

#include "JpegToBmpConverter.h"
#include "PngToBmpConverter.h"
namespace converter_test {
int calls = 0;
}
static bool convert(FsFile&, HalFile& o, int w, int h, bool) {
  ++converter_test::calls;
  BmpHeader b;
  createBmpHeader(&b, w, h, BmpRowOrder::TopDown);
  if (o.write(&b, sizeof b) != sizeof b) return false;
  uint8_t row[100]{};
  const size_t n = (w + 31) / 32 * 4;
  for (int y = 0; y < h; y++)
    if (o.write(row, n) != n) return false;
  return true;
}
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(FsFile& a, HalFile& b, int w, int h, bool c,
                                                         CooperativeCancellation cancellation,
                                                         BmpConversionDimensions* dimensions) {
  if (cancellation.requested()) return false;
  if (dimensions) *dimensions = {w, h};
  return convert(a, b, w, h, c);
}
bool PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(FsFile& a, HalFile& b, int w, int h, bool c,
                                                       CooperativeCancellation cancellation,
                                                       BmpConversionDimensions* dimensions) {
  if (cancellation.requested()) return false;
  if (dimensions) *dimensions = {w, h};
  return convert(a, b, w, h, c);
}
