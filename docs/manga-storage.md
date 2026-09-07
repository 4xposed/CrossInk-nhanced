# Manga storage adapter

`lib/MangaPanel/MangaBook.{h,cpp}` adapts the [Matcha binary format](manga-format.md)
to CrossInk's `HalStorage`/`FsFile` boundary. It supplies indexed content and image
paths to the reader, cover and library flows; those callers own UI and decoding.
The compatibility reference is Matcha commit
`61ca61ba86e3c5709a24d1b9c4f3cf2d41488012`; the existing `lib/MangaPanel/LICENSE`
retains its license.

## Ownership and calling contract

Library callers can select `OpenMode::Index` (validated page count/index only),
`OpenMode::Metadata` (index and optional metadata), or `OpenMode::Cover` (index and image
paths, without page data/metadata/TOC). The default `Reader` mode preserves the complete
reader contract. Non-reader modes do not allocate the page decoder buffer; nonempty
`loadPage()` calls fail there. Progress display uses `Index` so it never loads large
optional metadata merely to obtain the page count.

Create a `manga::MangaBook`, call `open(folder)`, and release its resources with
`close()` from the owning activity's exit path. Destruction also releases owned
buffers. `open()` first clears previous state, including on failure. Empty/null
folders fail safely; trailing slashes are normalized, and there is no fixed
folder-name or nesting-depth limit beyond available memory and the HAL/filesystem.
`isMangaFolder()` recognizes a regular `panels.idx` marker without interpreting
its contents. Opening the book validates its header and every declared index
record against the actual 64-bit `panels.dat` extent. Empty records may have any
unused offset, and an all-empty book may omit `panels.dat`.

`open()` and `pageImagePath()` accept an optional borrowed cancellation token.
Budgeted cover work polls it around index records, directory entries and direct
probes; cancellation closes current handles and stops before further discovery.
The token is not retained by the book.

No operation retains a file handle. All opened files and directory entries are
explicitly closed; a close failure makes a required read/path operation fail.
No two readers of the same file overlap inside the adapter. Callers must still
coordinate other components that access the same files.

- `readPageInfo()` copies an index record into caller storage.
- `loadPage()` returns borrowed format views into one reusable page buffer.
  **Any** subsequent `loadPage()`, `open()`, or `close()` invalidates those views,
  including failed calls. Copy anything needed longer. Every failed load clears
  the supplied output; callers must discard older copies and nested cursors too.
- Title, author and language views remain stable until `open()`/`close()`.
  Missing, malformed, unreadable or unallocatable metadata falls back to the
  folder basename and empty author/language. Legacy metadata without a language
  trailer remains valid. Encoded text widths and embedded NUL bytes are preserved;
  views are not C strings.
- `tocCount()` exposes a validated optional TOC. `readTocEntry()` borrows one
  reusable title buffer, invalidated by **any** subsequent TOC read/open/close.
  Failed reads clear the output. TOC access does not overwrite page or metadata
  views. Out-of-range chapter page indexes are preserved, as the converter's
  `--max-pages` option can leave them behind; navigation must handle them.
- Image paths are copied into caller buffers. `PathResult::Found`, `Missing`, and
  `Error` distinguish a resolved regular file, an absent image, and an observable
  operation/buffer failure. Missing/error clears a nonempty output buffer.

The adapter is foreground-owned and not thread safe. When the render task reads
borrowed page data, serialize `loadPage()`/`open()`/`close()` with `RenderLock` or
an equivalent boundary guaranteeing that no view is in use. Loading outside that
lock and later swapping reader state is unsafe: this adapter overwrites the same
backing page buffer. It has no ActivityManager dependency of its own.

## Images and compatibility policy

At open, probe canonical page zero with `.jpg`, `.jpeg`, `.bmp`, then `.png`.
A match establishes numeric `page_%04u` identity. Each page uses that same explicit
extension priority independently, so JPG/PNG/JPEG mixtures work and duplicate
extensions resolve deterministically. Missing full images stay missing at their
physical index: they never shift a neighbouring image onto another page's OCR,
panels or saved position. The common complete uniform family remains directory-free,
with at most four direct probes per request and no filename vector.

Browser panels-only output may keep just page zero plus crops, omit the final
full image, or retain selected middle overviews. All are supported unchanged. If
page zero is absent, the first image request classifies the directory once for
valid in-range canonical names; a sparse canonical family then uses direct probes.
Only genuinely noncanonical families use dense legacy root-image ordering.
Ignore hidden entries, directories, unsupported extensions and panel crop names
matching `p`/`P`, digits, underscore, digits, dot (regardless of their extension).
Accept case-insensitive `.jpg`, `.jpeg`, `.png`, `.bmp`. Preserve Matcha's pinning:
combined cover/copyright names first, cover-only next, copyright-only next, in
original directory order within each group. Natural-sort all remaining names
using CrossInk's shared `FsHelpers::naturalCompare`; equivalent names retain
original directory order. A failed filename retrieval returns `Error` rather
than silently shifting later pages forward. Reopen after modifying a book's
images; cached legacy ordering assumes the directory is stable while open.

A real `panels/` directory selects that crop layout. Otherwise use the older flat
layout, including when `panels` exists as a regular file. Probe `.bmp` before
`.jpg` **for each crop**, permitting mixed crop extensions. Once `panels/` is
selected, missing crops do not fall back to root crops, avoiding stale duplicates
from a different conversion. Missing overview images and panel-only books are
valid; panel counts/rectangles remain the format decoder's responsibility.

## Allocation and I/O tradeoffs

All owned allocations use the production `makeUniqueNoThrow`; failures log and
return a required-operation error or the documented optional fallback. There are
no allocating string/vector result paths, full index arrays or filename lists.
No buffer requires PSRAM.

| Owned storage | Bound and reason |
| --- | --- |
| Folder and full path scratch | `2 * L + 258` bytes for normalized folder byte length `L`; lifetime storage and one reusable path supporting a 255-byte filename. Variable depth rules out a fixed stack buffer. |
| Page data | Largest validated nonempty record, at most 32768 bytes, allocated once at open and reused. No per-page allocation or stack-sized page body. |
| Optional metadata | Declared, validated extent only, at most `8 + 65535 + 65535 + 2 + 65535 = 196615` bytes. Stable views need resident storage; untrusted trailing file size never controls allocation. OOM frees this optional buffer and uses the folder fallback. |
| Optional TOC title | Longest declared label, at most 65535 bytes, allocated once. The adapter streams up to 1000 entry headers and seeks over labels; it never loads a potentially 65 MB TOC as a whole. OOM disables optional chapters. |
| Legacy scan | Three 256-byte filename slots plus a small ordinal/index cursor, allocated on first legacy path request and reused. Larger than a sensible local task-stack scratch buffer. Page-zero canonical paths do not require it; missing-cover classification shares it with legacy discovery. |

The maximum page/metadata/TOC payload sum is 294918 bytes, plus paths, object/scan
storage and HAL wrapper overhead. This is a format-bound worst case, **not** a
claim that such a pathological book fits alongside the reader on C3. The required
page buffer is allocated before optional text. Extremely large optional fields
may fall back on C3 when contiguous memory is unavailable; normal converter
metadata is tiny. Do not claim a hardware heap improvement without measuring
free internal heap and the largest allocatable block with the complete reader.

The low-memory legacy fallback selects the next sorted filename by rescanning
entries. Sequential navigation costs one scan per next page; a backward or first
random jump can require repeated scans up to that page. This intentionally trades
SD I/O for bounded memory. The last selected filename and directory ordinal are
cached. TOC reads resume at a cached cursor for forward access and restart for
backward access, without retaining an offset table. No file handle stays open between calls.

HAL limitations remain explicit: `exists()` exposes no error reason, and
`openNextFile()` cannot distinguish ordinary end-of-directory from every low-level
I/O failure. A false existence probe is treated as missing. HAL wrapper allocation
failure is observable on the directory and is treated as `Error`. Short reads,
failed seeks, name retrieval failures and failed closes are observable failures.
The adapter does not provide a snapshot against concurrent SD file replacement.

## Verification

`MangaBookTest` uses original-converter golden files and real temporary POSIX
files behind an isolated HAL double. It enforces explicit closes and same-path
reader exclusivity, injects read/seek/open/name/directory/close failures, and
intercepts only global nothrow allocation entry points in the test executable.
Production uses the real `Memory.h`; no test hooks enter firmware code.

Run:

```sh
cmake --build build/task16-tests --target MangaBookTest -j 4
ctest --test-dir build/task16-tests -j 1 --output-on-failure -R MangaBookTest
```

Coverage includes original UTF-8 metadata/OCR/translation/TOC, missing/legacy
optional files, malformed indexes/pages, zero-length pages, actual data extents
above 4 GiB, canonical/legacy images and crop layouts, pinned/tied ordering,
reopen/view lifetime, OOM fallback, large encoded strings, 1 GiB trailing optional
file padding and a sparse 1000-entry TOC with 65535-byte labels. Sparse files keep
those extent tests fast and do not write their full logical size to disk.

Hardware acceptance remains pending; the foreground reader now uses this adapter. On X4/C3
and Sticky or X4 Pro/S3, open a deeply nested arbitrary-name manga, then canonical,
legacy, mixed-extension, sparse and panel-only variants. Confirm page/panel paths
and title/chapter fallbacks, repeat open/close and jumps, and inspect `MNG` error
logs plus internal free/max-allocation heap (and PSRAM on S3). The portable book files need no reset; clear only disposable manga pixel/thumbnail files
when a cold-render comparison is required. These tests do not establish on-device
image rendering, navigation, ghosting or dictionary parity.
