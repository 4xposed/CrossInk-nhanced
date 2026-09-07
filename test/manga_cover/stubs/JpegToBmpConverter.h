#pragma once
#include <BmpConversionDimensions.h>
#include <CooperativeCancellation.h>
#include <HalStorage.h>
namespace converter_test {
extern int calls;
}
class JpegToBmpConverter {
 public:
  static bool jpegFileTo1BitBmpStreamWithSize(FsFile&, HalFile&, int, int, bool, CooperativeCancellation = {},
                                              BmpConversionDimensions* = nullptr);
};
