# Task 8a — renderer-free cancellable decode

Implemented Task 8a only. No worker, activity integration, SDK edits, PlatformIO
runs, commits or pushes were performed by this task. The SDK gitlink remains
`1e8ee543edca397f2b8747811f5a88f1bc35d233`.

## Interface and behavior

- `lib/CooperativeCancellation/CooperativeCancellation.h` defines the shared
  borrowed function-pointer/context contract. Existing callers omit it.
- `CacheDecodeConfig` in `ImageToFramebufferDecoder.h` supplies exact positive
  output geometry, explicit screen extents, path and the existing rendering
  policy. The cache-only entry validates contained geometry (axes up to 2048).
  Other decoder implementations retain a default unsupported result.
- JPEG and PNG foreground/cache-only entry points share the original scaling,
  quantization and Bayer-origin code. The cache-only branch receives a null
  renderer and guards every DirectPixelWriter initialization/row/pixel access.
  It does not ask for renderer dimensions, orientation, framebuffer or mode.
- Cancellation is checked before setup, each JPEG draw block or PNG source row,
  after decoding, each streamed cache row, and after final sync/close. JPEG draw
  cancellation is sticky because the real JPEGDEC library can return success
  after callback abort. Cancellation is never disabled by completion percentage.
- Cache-only success requires startup, writes, final sync and close to succeed.
  Failed streams remove temporary output; foreground cache failures retain BW
  rendering. Real-codec malformed-source tests exposed leaked handles after
  decoder open/header failure; those returns now explicitly close the source,
  including dimension-probe failures.
- Manga fingerprint, source identity, configure, open/validation and publication
  accept optional cancellation. Fingerprint and payload CRC loops poll every
  256-byte chunk. Cancelled fingerprints do not replace identity fields.
  Cancelled validation closes its payload. Publication polls before replacing
  the pair, then finishes the short existing two-rename transaction without an
  intervening poll; failure removes temporary/incomplete output. It remains a
  recoverably validated pair, not an atomic filesystem transaction.
- BMP probe/production accept the same callback, poll before setup and between
  source rows, and clean failed/cancelled output after checked sync/close.

## Ownership and memory

No second framebuffer, image-sized allocation or PSRAM assumption was added.
The renderer pointer is borrowed only by foreground decode. Config/path and
cancellation context must live until the synchronous entry returns. Producers
still require exclusive source/cache ownership; these APIs do not supply SD
lifetime arbitration. Task 8b must enforce that ownership before enabling a
worker and must reject stale generations before publication.

Decoder objects retain job-scoped fallible allocation (`makeUniqueNoThrow`):
roughly 20 KiB JPEG and 44 KiB PNG estimates are the existing admission figures,
not a measured total working-set guarantee. Real PNG configuration uses
`PNG_MAX_BUFFERED_PIXELS=16416`. PNG additionally owns its source-width grayscale
row, at most 2048 bytes. These cannot fit the small task stack and cannot be
static/shared between concurrent decodes. Decoder objects are freed before
cache finalization as in the prior foreground path.

PixelCache replaces manually owned malloc storage with one fallibly allocated
unique byte array. The same band calculation is retained: at most 24 KiB plus
one packed spare row (up to 512 bytes for supported cache-only geometry), with
at least 16 rows when geometry permits. JPEG bands must hold the tallest scaled
MCU block. Allocation happens once per decode, never per pixel or row. The
array is automatically freed on every return. Existing file-handle objects
transfer ownership explicitly to the C decoder callbacks and are deleted on
close. Callback/context adds two pointers, with no allocation. Manga checksum
scratch remains a 256-byte stack buffer; BMP scratch remains caller-owned,
worst case 9216 bytes. No heap/latency improvement or safe worker memory budget
is claimed; Task 8b still needs internal free/largest-block and task-stack
admission accounting.

## Verification

Offline native setup (local GoogleTest and codec sources only):

```sh
cmake -S test -B /private/tmp/crossink-prefetch-tests \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/private/tmp/crossink-manga-pixel-build/_deps/googletest-src
cmake --build /private/tmp/crossink-prefetch-tests \
  --target ImageCacheDecodeTest MangaPixelCacheTest MangaBitmapPixelsTest -j2
ctest --test-dir /private/tmp/crossink-prefetch-tests \
  -R 'RealCodecs|PixelCacheTest|MangaBitmapPixelsTest' --output-on-failure
```

Result: **39/39 passed**, sequential CTest. Build and output are recorded in
`/private/tmp/crossink-prefetch-final-build.log` and
`/private/tmp/crossink-prefetch-final-tests.log`. The initial malformed-header
regressions failed for both real codecs with one leaked source handle; red
output is `/private/tmp/crossink-prefetch-codec-red.log`.

`test/image_cache_decode` compiles the actual locally installed JPEGDEC.cpp,
PNGdec.cpp and PNGdec's inflate/checksum C sources. Only HAL, renderer, logging,
heap-admission and time services are host doubles. No simulator decoder stub is
used. The target skips explicitly if local codecs are absent and supports
`IMAGE_CODEC_DEPS` to point to another existing installation; it never downloads
codecs. Third-party codec headers produce host compiler warnings.

The 20 real-codec cases cover full-byte foreground/cache-only parity at four
sizes (1:1, two downscales and upscale), both dither policies and all four
renderer orientations; zero renderer access in cache-only calls; cancellation
at every observed callback/row/finalization poll, explicit early/middle/90%
written output, cancellation on final sync, startup/header/midstream/final-row
write failures, sync/close failures, foreground BW fallback, invalid geometry
and malformed header handle cleanup. Sixteen golden hashes additionally compare
against the **original pre-Task-8a foreground source snapshot**, compiled with
the same real codecs. The fixture README records provenance; the temporary
baseline runner is `/private/tmp/prefetch-baseline.cpp`.

The 12 manga-cache cases cover identity/CRC/corruption compatibility and chunk
cancellation without changing identity, losing an existing published pair or
leaking a reader. The 7 BMP cases cover pixel mapping/row order, output failures
and cancellation before/between rows. `git diff --check` passes; touched C++ is
clang-formatted. Whole firmware builds remain the parent's responsibility.

## Hardware acceptance still required

On X4, compare cold JPEG/PNG and BMP manga pages/panels after clearing only the
relevant disposable `/.crosspoint/manga_<folder-crc>/` pixel cache. Check framing,
dither origin and four gray levels against existing foreground rendering, and
verify repeated malformed images or interrupted writes do not leave duplicate
reader errors or valid partial caches. After Task 8b integration, cancel large
images by rapid navigation, rotation, lookup/settings, exit and sleep; expect
responsive polling and released files before foreground work starts. Record
internal free/largest heap and worker stack watermark, and repeat on an S3
profile. These native tests do not prove physical SD exclusion or latency.
