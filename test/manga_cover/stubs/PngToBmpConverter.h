#pragma once
#include <BmpConversionDimensions.h>
#include <CooperativeCancellation.h>
#include <HalStorage.h>
class PngToBmpConverter {
 public:
  static bool pngFileTo1BitBmpStreamWithSize(FsFile&, HalFile&, int, int, bool, CooperativeCancellation = {},
                                             BmpConversionDimensions* = nullptr);
};
