# Manga cover thumbnails

The library uses page zero as the cover. It does not fall back to panel crops. JPEG and PNG covers are streamed through the existing one-bit converters; BMP covers are read and scaled one row at a time. Thumbnail generation never allocates a framebuffer.

Cached thumbnails live at `/.crosspoint/manga_<folder-crc>/thumb_v3_<width>x<height>.bmp`. A zero width requests the conventional 2:3 cover ratio. Both axes are limited to 800 pixels, the largest axis of current CrossInk displays, which bounds BMP scratch memory even if a caller supplies bad dimensions.

The adjacent `.src` record contains the resolved source-path CRC, a streamed source-content CRC, source size, and requested dimensions. The checksum uses one bounded 256-byte stack buffer. The BMP signature, exact generated pixel offset, header and image lengths, dimensions, bit depth, compression, palette count, and complete file length are checked before reuse.

Generation writes and syncs temporary files, validates the complete BMP, closes all handles, and only then renames them into place. Failed and incomplete temporary output is removed.

Version 3 uses the explicit 40-byte MCG3 layout in [File formats](file-formats.md).
The sidecar binds exact emitted dimensions and a complete BMP CRC to the source
and request identity. New output dimensions are checked against codec-reported
source geometry, including progressive JPEG eighth-scale rounding. Warm hits and
primary, backup, or crossed recovery candidates require a matching BMP digest;
filenames alone cannot establish a pair. CRC reads use the existing 256-byte
cooperative buffer. Cancellation while validating candidates leaves prior files
untouched. Old v2 caches are disposable and regenerate under the new basename.

Full sleep-cover preparation returns the validated or newly published cache path
along with its result. If its cooperative budget expires, the sleep screen uses
the default/custom fallback without reopening the manga index or probing codec
dimensions again. Index and image discovery share the same cancellation token.
The 2500 ms policy is not a hard SD-operation deadline or a measured hardware
guarantee. Physical validation should record the logged stage, elapsed time and
maximum poll gap on a slow disposable C3/S3 book.
