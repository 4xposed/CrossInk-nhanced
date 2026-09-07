# Manga pixel cache

Manga pages use the image decoder's existing raw 2-bit output without changing
that private format. The raw file starts with little-endian `uint16_t` width and
height, followed by tightly packed rows. Each byte stores four pixels, most
significant pair first, and each row occupies `(width + 3) / 4` bytes.

`MangaPixelCache` publishes that raw file as a validated pair in the book's
`/.crosspoint/manga_<folder-crc>/` directory:

- `pixels_v1_p<page>_<panel>.pxc` contains the decoder output.
- `pixels_v1_p<page>_<panel>.pxc.id` is a 48-byte, explicitly little-endian
  version-1 envelope.

The envelope stores the source path CRC, source content CRC, 64-bit source size,
source and output dimensions, output position, screen dimensions, orientation,
pixel-policy flags, and the raw payload CRC. Bytes 42 and 43 are reserved and
must be zero. The identity is serialized field by field; compiler padding and
host byte order never enter the disk format.

The source content CRC means replacing an image with different bytes but the
same length invalidates the entry. The payload CRC catches corruption that does
not change file length. A cache is opened for drawing only after the sidecar,
identity, raw dimensions, exact payload length, and the complete streamed
payload checksum have all passed. Validation uses a fixed 256-byte stack buffer.
Rendering then reads one caller-owned row buffer at a time and can rewind to the
first payload row without reopening another file.

`MangaPixelCache::sourceIdentity()` reads and validates only the exact sidecar
envelope. It lets the reader recover source dimensions before probing the image
decoder, then compare the current source fingerprint with the cached path CRC,
content CRC, and size. A successful identity lookup does not authorize drawing:
the caller must still configure the desired render geometry and call `open()` so
the raw dimensions, length, and payload CRC are validated before any row is used.

Publication uses `.tmp` files. The decoder writes the raw temporary path returned
by `temporaryPath()`. `publish()` validates, syncs, and closes that file, writes
and syncs the temporary sidecar, removes any previous pair, and renames both files.
Any failure removes temporary and partially published files so the request can
be retried. This includes failure of the second rename: the disposable old cache
may be lost, but no invalid pair remains openable and rendering regenerates or
uses its black-and-white fallback. `configure()`, `close()`, destruction, and failed validation all
close the reader explicitly; code never keeps two handles to the same pixel file.

Target and screen axes are limited to 2048, which comfortably bounds current
800×480 output and keeps row and payload arithmetic small. Source dimensions
may be larger because JPEG sources commonly reach 4096×3072. Source width is
limited to 4096, source height to 3072, and decoded source geometry to 2048×3072
total pixels. Pages are limited to 0–9999 and panels to -1–254. All size calculations use 64-bit
arithmetic. The four fixed 128-byte path buffers are safe because the variable
book path is represented by the short folder CRC in the cache directory name;
the page and panel suffixes are bounded numeric fields.

On hardware, test a grayscale manga on an X3/X4 and a Sticky or X4 Pro. Open a
page cold, return to it for a warm-cache draw, and compare framing and gray
levels. Replace the source image with different bytes of the same size and check
that the page regenerates. Remove or truncate either cache file and confirm the
reader falls back to a usable black-and-white page without partial gray drawing.
Repeat page turns while watching free heap, largest allocatable internal block,
and (on S3) PSRAM statistics; no second framebuffer is expected.
