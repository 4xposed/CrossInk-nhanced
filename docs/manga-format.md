# Manga binary format foundation

The decoder in `lib/MangaPanel/MangaFormat.{h,cpp}` accepts Matcha Reader's
version 2 panel format and version 1 metadata/TOC. The compatibility reference is
Matcha commit `61ca61ba86e3c5709a24d1b9c4f3cf2d41488012`. Its MIT license is
retained in `lib/MangaPanel/LICENSE`. The [MangaBook storage adapter](manga-storage.md)
uses this decoder through CrossInk's HAL. The foreground manga reader opens these
folders from Books and integrates validated render caches, idle prefetch and
shared dictionary lookup of stored OCR. These firmware features leave the portable
book files unchanged.

All integers are unsigned little endian. Strings carry byte lengths, not character
counts, and are not NUL terminated. UTF-8 bytes (including embedded NUL) and all
rectangles are preserved; the decoder performs no text normalization, coordinate
clamping, sorting, image scaling, or UTF-8 repair.

## `panels.idx`

| Field | Bytes | Contract |
| --- | --- | --- |
| version | 4 | Exactly 2 |
| pageCount | 4 | 1 through 10000 |
| Each page's dataOffset | 4 | Byte offset into `panels.dat` |
| dataLength | 4 | 0 through 32768 |
| imageWidth, imageHeight | 2 each | Source image dimensions, including zero |

Each page record is 12 bytes. `decodeIndexHeader` and `decodeIndexRecord` support
reading the index on demand without retaining 10000 records. `decodeIndex` is a
convenience validator for a complete caller-owned index buffer; it validates all
declared records against the supplied complete data-file size before publishing
the header. Nonempty records must fit within that extent, checked without an
`offset + length` overflow. Zero-length records ignore their unused offset, matching
upstream's early return without seeking or reading the data file. Overlapping or
out-of-order records are allowed. The caller must separately verify that SD reads
return the requested number of bytes.

## `panels.dat`

A zero-length page is valid and contains no panels. A nonempty page starts with a
one-byte panel count (0–255) and a reserved byte. Each panel consists of:

| Field | Bytes |
| --- | --- |
| x, y, w, h | 2 each |
| textCount | 1 |
| reserved | 1 |
| translationLength | 2 |
| translation | translationLength |
| Each OCR block: x, y, w, h | 2 each |
| textLength | 2 |
| text | textLength |

The translation precedes the 0–255 OCR blocks. Panel and OCR rectangles use the converted page image's coordinate space. The
binary decoder preserves their values even when a particular writer emitted an
out-of-bounds rectangle (including the pinned writer's OCR endpoint/size defect). Panel and OCR order remains exactly as encoded.
`decodePage` validates every panel and text block before returning a `PageView`.
Its panel cursor yields `PanelView`s containing a translation view and text cursor.
All cursors are bounded by the validated record region and declared count.

## `meta.bin`

Header: version (4 bytes, exactly 1), titleLength (2), authorLength (2), followed by
title bytes then author bytes. Legacy metadata ends here. When additional bytes
exist, the next two bytes are languageLength, followed by the language bytes.
Each length may span the full uint16 range; this allocation-free layer imposes no
arbitrary shorter language/title limit. The storage adapter budgets its buffers
separately and falls back if optional metadata cannot be allocated. No language
aliasing or normalization occurs while decoding.

## `toc.idx`

Header: version (4 bytes, exactly 1), entryCount (4, 0–1000). Each entry contains
pageIndex (4), titleLength (2), and title bytes. The decoder preserves ordering,
duplicate indexes, empty titles, and indexes beyond the currently converted page
count. The latter can occur with the original converter's `--max-pages` option;
the reader rejects unreachable chapter selections without rewriting portable entries. TOC version 1 is
explicitly checked even though the reference reader does not check this field.

## Ownership, limits, and failures

`Bytes` is `std::span<const uint8_t>`. Views store spans, counts, and
`std::string_view`s, never owned strings or vectors. The decoder allocates no heap
memory, retains no global mutable state, uses only small local records, and needs
no framebuffer or PSRAM. It reads integers byte by byte, so unaligned buffers are
safe. Decoding validates once and iteration scans records linearly; panel iteration
also validates the panel's OCR extent before yielding it. There is no recursion.

The caller must keep backing bytes alive and immutable for the lifetime of every
derived view/cursor. Loading another page into a reused buffer invalidates all old
views. Copying a cursor permits independent iteration without copying text.

Every failed decoder/cursor call clears its output. A failed cursor call leaves
its cursor position unchanged; exhaustion returns `Error::End` and clears the
output. `errorName` provides fixed diagnostic strings; HAL callers must
log failures with the file and page context. This layer has no HAL/log dependency.

Reserved bytes and trailing extension bytes are ignored for compatibility. An
optional metadata language trailer that begins but is truncated rejects the whole
metadata file, unlike the reference reader which can retain partial title/author
state or ignore malformed language. Malformed metadata/TOC publishes no partial
output. The reader falls back to the folder title and percentage navigation
when those optional files cannot be decoded. Missing optional files are a storage
concern, not empty valid binary files.

## Reproducible checks

`test/manga_format/fixtures/generate.py` imports only the **original pinned**
converter, verifies its checkout SHA and unchanged script contents against Git,
and invokes its binary writers with literal inputs. `fixtures/README.txt` records
the script SHA256 and output hashes. The checked-in fixtures do not depend on the
ported converter, optional image/OCR packages, network calls, or credentials.

```sh
cmake -S test -B build/task16-tests -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
cmake --build build/task16-tests --target MangaFormatTest -j 4
ctest --test-dir build/task16-tests -j 1 --output-on-failure -R MangaFormat
```

Tests cover original-writer records and Japanese text, embedded NUL, legacy and
language metadata, TOC order, required-field prefix truncations, maximum counts
and lengths, reserved/trailing bytes, unaligned buffers, malformed cursors,
overflowing data extents, and zero-length pages. Initial behavior tests were run
against failing decoder stubs before implementation; the zero-length offset
compatibility correction also has a separately observed red/green regression.

Final on-device verification of the integrated reader remains pending:
open these fixtures on X4/C3 and Sticky or X4 Pro/S3, check decoded
page/panel order, Japanese OCR and translation, and ensure malformed optional
files produce logs and fallback navigation. No rendered-image parity or hardware
memory/performance claim follows from these native parser tests.
