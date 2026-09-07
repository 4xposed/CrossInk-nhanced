These are original, deterministic test images, generated offline for the native
real-decoder tests. `pattern.png` is 129 × 193, RGB8, non-interlaced. Each channel
is `(x * 7 + y * 11 + channel * 53) % 256`, emitted as PNG filter-0 rows with
Python's standard-library zlib. `pattern.jpg` was encoded once from that PNG
using macOS `sips -s format jpeg`. Running tests needs neither Python nor sips.

`foreground-fnv64.txt` records 64-bit FNV-1a of complete raw pixel-cache bytes
(header included), generated using the pre-Task-8a foreground converter sources
archived at `/private/tmp/crossink-prefetch-before`, with real local JPEGDEC and
PNGdec sources. Columns: format, output width, output height, dithering, hash.
Origin is (7,9), screen extents 512×512, exact dimensions enabled. These hashes
check compatibility with the original foreground output independently of the
new cache-only implementation. The runtime tests additionally compare the full
bytes against foreground rendering in every orientation.
