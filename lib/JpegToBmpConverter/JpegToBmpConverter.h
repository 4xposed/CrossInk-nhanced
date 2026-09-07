#pragma once

#include <BmpConversionDimensions.h>
#include <CooperativeCancellation.h>
#include <HalStorage.h>

class Print;
class ZipFile;

class JpegToBmpConverter {
  static bool jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool crop = true, bool adaptiveContain = false,
                                          CooperativeCancellation cancellation = {},
                                          BmpConversionDimensions* sourceDimensions = nullptr, bool imageLevels = false);

 public:
  static bool jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, bool crop = true, bool imageLevels = false);
  // Convert with custom target size (for thumbnails)
  static bool jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                          bool adaptiveContain = false, CooperativeCancellation cancellation = {},
                                          BmpConversionDimensions* sourceDimensions = nullptr);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  static bool jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                              bool adaptiveContain = false, CooperativeCancellation cancellation = {},
                                              BmpConversionDimensions* sourceDimensions = nullptr);
};
