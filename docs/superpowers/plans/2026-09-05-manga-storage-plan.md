# MangaBook HAL adapter implementation plan

> Use executing-plans/subagent-driven-development. Work in `matcha_features` in place;
> no commits or pushes. Preserve the completed dictionary and manga foundation.

## Scope and design

Task 4 of `2026-09-05-manga-port-implementation.md`, implementing the storage
boundary from `../specs/2026-09-05-manga-port-design.md`. This makes converter-produced
folders accessible to firmware code. It does not yet expose a manga activity or
library row, generate thumbnails, or create caches.

Files: `lib/MangaPanel/MangaBook.{h,cpp}`, `test/manga_book/*`, test registration in
`test/CMakeLists.txt`, and `docs/manga-storage.md`. Existing format decoder and
original-converter fixtures are dependencies; dictionary test stubs stay unchanged.

## Binding requirements

- Use CrossInk `Storage`/HAL file APIs only. Explicitly close every handle, including
  failure paths, as AGENTS requires. The older HAL skill's destructor-only advice
  does not apply. Do not change the SDK gitlink.
- Open an arbitrary-depth folder by its `panels.idx` marker, with safe handling for
  empty/root/trailing-slash paths. Do not infer book type from the folder name.
- Validate the index and `panels.dat` extent before page reads. Read index records on
  demand rather than keeping 10000 records in RAM. Empty records ignore offsets.
- Own one reusable fallibly allocated page buffer, sized to the maximum declared
  page payload at open, at most 32768 bytes. Return borrowed validated page views.
  A later load invalidates old views; failed loads publish no stale output.
- Optional metadata and TOC errors log and fall back safely. Preserve UTF-8 bytes,
  full encoded field widths, legacy metadata without language, and TOC labels/indexes.
  Do not allocate from an arbitrary file size before validating declared structure.
- Stream optional TOC data rather than buffering a potentially 65-MB file. Any offset
  table/title buffer must have explicit bounds, fallible allocation and ownership.
- Prefer canonical page naming `.jpg`, `.bmp`, `.png` based on first/last probes.
  Generate paths without a filename vector or full directory walk in that path.
- Legacy layout: ignore directories, dot files and panel crop filenames; prioritize
  the `cover` flag then the `copyright` flag, retaining directory order within each
  pinned priority group, and natural-sort ordinary images. A name containing both
  words precedes a cover-only name, matching the pinned source's comparator.
  Use bounded memory; document any repeated-scan cost for old layouts.
- Resolve both `panels/p<page>_<panel>` and old flat crop layouts, BMP/JPEG. Missing
  images must be distinguishable from successful path resolution; never use a crop
  as an overview/cover accidentally.
- No new renderer, framebuffer, PSRAM assumption, durable store, cloud dependency or
  dictionary changes. Do not expose test hooks in production classes.

## Steps and evidence

- [x] Inspect pinned reader index/scan/crop behavior and actual HAL methods.
- [x] Add failing native behavior tests using original-writer fixtures and controlled
  filesystem failures; cover opening, metadata, TOC, ordering and image layouts.
- [x] Implement the adapter with bounded owned buffers and explicit close checks.
- [x] Test repeated reopen/close, zero-length and truncated records, missing images,
  optional-file failure, read/seek/close failure, directory allocation failure,
  output invalidation and handle lifetime. Inject allocation failure at the allocator
  boundary if practical, without adding production-only-for-tests APIs.
- [x] Compile against actual firmware HAL headers using installed C3/S3 toolchains,
  and run native tests sequentially plus sanitizer checks appropriate to this code.
- [x] Independently review code and test evidence, fix blocking findings, document
  API lifetimes, memory bounds, fallback sorting cost and hardware limitations.
- [x] Record final results in the main plan/validation ledger and verify Git status.

Results: full native suite **538/538 passed**, including **21 adapter tests**;
separate ASan/UBSan **21/21 passed**; C3 and Sticky/S3 actual HAL-header compilation
passed with warnings as errors. LeakSanitizer is unavailable on this Darwin runtime.
Review resolved a failed-filename-read case that could shift legacy image indexes;
no blocking findings remain. No dictionary/SDK changes, commits, pushes or uploads.

## Hardware verification after reader integration

On X4/C3 and Sticky/X4 Pro/S3, open the same manga in canonical and legacy layouts,
then open a nested arbitrary-named folder and panel-only book. Verify page/panel
order and metadata/TOC, remove an overview image, and try truncated optional/index
files. Expect a supported fallback or logged error without crash, stale state, or
leaked handles. Monitor internal free/largest heap across repeated opens and turns.
No cache reset is required for this storage-only adapter; no new cache is written.
