#include <JPEGDEC.h>

#include <cstddef>
// Keep vendor headers separate: JPEGDEC and PNGdec define incompatible macros.
namespace manga {
size_t jpegPrefetchDecoderBytes() { return sizeof(JPEGDEC); }
}  // namespace manga
