# Manga storage adapter independent review

Date: 2026-09-05. Scope: Task 4 only, governed by
`2026-09-05-manga-storage-plan.md`.

Result: **No unresolved blocking findings** in the reviewed adapter, isolated native
tests, or `docs/manga-storage.md`. This is not approval of future reader, rendering,
cache, prefetch, or dictionary integration.

## Finding resolved during review

The initial legacy directory scan silently filtered a regular entry when
`getName()` returned zero. A failed read of `1.jpg` could therefore select `2.jpg`
successfully for page zero, shifting the image-to-panel relationship.

The implementation now treats invalid filename retrieval as failure, explicitly
closes both entry and directory, and returns `PathResult::Error` with empty output
(`lib/MangaPanel/MangaBook.cpp:369`). The implementation agent reproduced the
incorrect `2.jpg` result before the fix. The regression
`FailedFilenameReadCannotShiftLegacyPageIndexes`
(`test/manga_book/MangaBookTest.cpp:366`) checks error, empty output and no live
handles. Re-review confirms the failure propagates without advancing the cached
legacy cursor.

## Reviewed contracts

- Every operation releases file handles explicitly, including failed opens,
  failed reads/seeks, decode failures and directory-entry failures. Required
  operations reject close failures (`MangaBook.cpp:20`). Index and data reads are
  sequential; the adapter does not overlap readers of the same file.
- Index records are streamed and validated against 64-bit data extents at open.
  Subsequent reads reject records exceeding the existing page allocation
  (`MangaBook.cpp:100`, `MangaBook.cpp:130`). Empty records preserve unused offsets.
  Failed page/record/TOC calls clear supplied outputs before returning.
- Actual CrossInk `HalStorage.h` and `HalStorage.cpp:437`/`:445` support the used
  close and directory-allocation error contracts. The test double correctly puts
  directory-wrapper allocation failure on the parent directory. The adapter
  documents HAL's existence/EOF observability limits rather than inventing APIs.
- The real `Memory.h` performs fallible allocations. Page storage is bounded at
  32768 bytes; validated optional metadata at 196615 bytes; one reusable TOC label
  at 65535 bytes. No allocation follows arbitrary trailing file size, and the TOC
  is not loaded wholesale. Optional OOM falls back safely. The legacy scan owns
  three filename slots rather than an unbounded filename vector.
- Original-writer fixtures exercise UTF-8 OCR/translation and optional metadata,
  legacy metadata without language, TOC indexes and full encoded text widths.
  Tests also cover malformed/truncated records, zero data, post-open corruption,
  and sparse data crossing the 32-bit offset boundary.
- Canonical paths preserve first/last `.jpg`, `.bmp`, `.png` priority and never
  substitute another interior page. Marker detection uses a regular `panels.idx`
  rather than folder naming. Legacy ordering preserves cover/copyright priority
  and directory order within pinned groups, uses shared natural comparison, and
  excludes directories, hidden entries and panel crop names. Both crop layouts
  resolve BMP/JPEG per crop; selecting a real `panels/` directory is a per-book
  choice, as documented.
- Borrowed page/metadata/TOC lifetimes and foreground ownership are explicit in
  `MangaBook.h:8` and `docs/manga-storage.md`. Future renderer integration must
  serialize buffer mutation while a render task holds a view. No second
  framebuffer, renderer API or SDK change was introduced for this adapter.

The compatibility comparison used the unchanged local Matcha source at
`61ca61ba86e3c5709a24d1b9c4f3cf2d41488012`; CrossInk's CodeGraph was consulted
first. The SDK pin remains `1e8ee543edca397f2b8747811f5a88f1bc35d233`.

## Validation evidence and limits

The implementation agent reported **21 MangaBook tests passing** after formatting
and the filename failure fix. This reviewer inspected the tests and HAL double;
the suite was not redundantly rerun. Allocation tests intercept only test-binary
global nothrow operators while retaining production `Memory.h`; they cover
required and optional OOM, 1 GiB optional-file padding, and 1000 maximum-size TOC
labels. There are no production test hooks. The parent agent owns full native,
sanitizer and actual firmware compile results in the validation ledger.

Hardware acceptance remains pending reader integration: on X4/C3 and Sticky or
X4 Pro/S3, verify canonical/legacy/panel-only paths, optional-file fallback and
repeated opens/jumps with internal free/largest-block heap logging. No cache
reset is needed for this adapter. Legacy random/backward access intentionally
trades repeated SD scans for bounded memory; no latency claim is made.

Review work edited only this report. Dictionary work and SDK files were preserved;
no commits or pushes were performed.
