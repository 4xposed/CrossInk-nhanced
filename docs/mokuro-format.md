# CrossInk Mokuro book format v3

One reading unit is one cropped panel. Images are named `page_0000.bmp`, `page_0001.bmp`, etc. Each is a standard uncompressed 1-bit BMP (black palette entry 0, white entry 1), aspect-fitted inside the selected profile: X3 528x792, X4/X4 Pro 480x800, with the bounds swapped for landscape crops. Width/height are actual image dimensions; no stretching or padding is implied. Integers below are unsigned little endian. Files are immutable after publication. New exports default to Floyd–Steinberg dithering; `--dither bayer` retains the previous method. This changes pixel content only, not the v3 wire format.

Version 3 permits landscape dimensions, preserving source orientation and fitting before dithering. Version 2 expanded the permitted image width for X3. Binary field layouts and OCR limits are unchanged. Readers accept versions 1, 2, and 3, applying each version's limits. New exports use version 3 for all profiles; older firmware rejects them explicitly and must be updated. The `CMI1` magic identifies the format family, not its version.

## book.mki

12-byte header: ASCII `CMI1`, u32 version=3, u32 panelCount (1..10000).
Exactly panelCount 20-byte records follow:

| Field | Bytes |
|---|---|
| OCR offset in book.mkd | 4 |
| OCR byte length | 4 |
| Image width, height | 2 each |
| Original physical page, zero based | 4 |
| Panel within original page, zero based | 2 |
| Reserved (zero) | 2 |

Image dimensions must be positive and fit within 528x800 or 800x528 for version 3. Version 2 permits only 528x800; version 1 permits only 480x800. Exporters apply the tighter selected device bounds, preserve aspect ratio, and do not enlarge low-resolution sources. Image pixels and OCR coordinates retain the original orientation; the reader automatically rotates landscape views. Each OCR extent must fit within book.mkd; records are contiguous in output. No full index needs to be held in firmware memory.

## book.mkd

Each panel record starts with u16 blockCount (0..255) and u16 reserved=0. Each block is u16 x,y,width,height, u16 UTF-8 byte length, u16 flags (bit 0 = vertical; all other bits zero), then exactly that many text bytes. Rectangles have positive area and fit within the associated image. Text must be valid UTF-8 without NUL. Maximum panel record length is 32754 bytes. Oversized content fails export, never truncates silently. Line text is joined with newline in this block-level representation; original line strings/polygons remain in the master Mokuro output.

## Metadata and source mapping

`meta.bin` uses the existing simple metadata v1 encoding (u32 1, u16 titleBytes, u16 authorBytes, title, author, u16 languageBytes, language), with writer limits of 1024 title bytes, 1024 author bytes and 16 language bytes. Language defaults to ja. This shares metadata encoding, not the Matcha panel format.

`manifest.json` is a host-readable version=1 manifest containing source page relative paths and hashes, source dimensions, crop rectangles, original page/panel ordinals, exported dimensions, preparation settings, and a `device` export profile (`x3`, `x4`, or `x4-pro`). The optional `dither` field records `floyd-steinberg` or `bayer`; older exports omit it. It is not parsed during firmware page navigation. Source crop images and original `.mokuro` stay in a sibling work directory; final output contains only files needed for reading plus this manifest.

## Firmware integration

MangaBook recognizes book.mki and reads book.mkd on demand. A bounded adapter presents each record as one full-image panel through the existing PageView interface, allowing the existing activity/dictionary/cover infrastructure to consume the new format without loading JSON. Original page mapping stays in the index for subsequent UI work; initial navigation counts reading units. This is a transitional consumer, not a claim that the full replacement UI is finished.

The adapter must not allocate a second maximum-size page buffer. It streams blocks into its existing owned page buffer, adding the small legacy view header in memory only. It validates flags, rectangles, counts, extents and reads before publishing a view. Malformed records fail with logs. No file handle survives an operation.

## Version and testing policy

Change the version before changing the wire layout. Fixtures must include independently specified expected bytes and malformed/truncated records. Verify Rust exports against the native C++ reader. Physical X4 acceptance must verify image/OCR alignment, dictionary selection, repeated open/close, sleep/resume, and heap/stack headroom. No hardware claims follow from host tests.
