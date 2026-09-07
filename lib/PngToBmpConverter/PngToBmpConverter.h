#pragma once

#include <BmpConversionDimensions.h>
#include <CooperativeCancellation.h>
#include <HalStorage.h>

class Print;

class PngToBmpConverter {
  static bool pngFileToBmpStreamInternal(FsFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight, bool oneBit,
                                         bool crop = true, bool adaptiveContain = false,
                                         CooperativeCancellation cancellation = {},
                                         BmpConversionDimensions* sourceDimensions = nullptr, bool imageLevels = false);

 public:
  static bool pngFileToBmpStream(FsFile& pngFile, Print& bmpOut, bool crop = true, bool imageLevels = false);
  static bool pngFileToBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                         bool adaptiveContain = false, CooperativeCancellation cancellation = {},
                                         BmpConversionDimensions* sourceDimensions = nullptr);
  static bool pngFileTo1BitBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                             bool adaptiveContain = false, CooperativeCancellation cancellation = {},
                                             BmpConversionDimensions* sourceDimensions = nullptr);
};
