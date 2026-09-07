# Matcha manga port integration design

The binding feature inventory is [the handoff](../plans/2026-09-05-manga-port-handoff.md).
Work is authorized in the existing `matcha_features` checkout. Do not commit, push,
replace dictionary work, modify the reference checkout, or rebuild CodeGraph.

## Architecture

Port incrementally at CrossInk's existing boundaries. A direct activity copy would
couple Matcha's renderer, stores, and dictionary implementation to CrossInk; replacing
CrossInk subsystems would discard accepted behavior. Instead retain the interoperable
book format and converter, and adapt the reader to CrossInk's HAL, ActivityManager,
UITheme, stores, and unified dictionary engine.

Start with a standalone, allocation-free binary decoder in `lib/MangaPanel`.
It consumes byte spans, validates lengths before access, and returns non-owning views.
A future MangaBook storage adapter owns one fallibly allocated page buffer (maximum
32768 bytes, matching the pinned reader), closes every file explicitly, and keeps
views alive until navigation invalidates them. This avoids vector/string amplification
on C3. Index records can be read on demand instead of retaining 10000 records.
No second framebuffer or PSRAM requirement is introduced.

The first implementation slice establishes the decoder and original-converter
fixtures, plus the host converter with offline tests and setup documentation. It does
not expose manga folders in the UI before a functional reader is available.

## Compatibility and source evidence

Reference: Matcha `61ca61ba86e3c5709a24d1b9c4f3cf2d41488012`, MIT; retain its
copyright/license with copied code. Its working tree has only an untracked CodeGraph
index. CrossInk SDK remains `1e8ee543edca397f2b8747811f5a88f1bc35d233`.

- `../matcha-reader/lib/MangaPanel/MangaPanel.cpp:218`: only panel version 2
  accepted; 1–10000 pages, 12-byte records, little endian.
- `../matcha-reader/lib/MangaPanel/MangaPanel.cpp:369`: zero-length page records
  are valid; nonempty data is limited to 32768 bytes. Translation precedes OCR blocks.
- `../matcha-reader/lib/MangaPanel/MangaPanel.cpp:470`: optional chapter index.
- `../matcha-reader/lib/MangaPanel/MangaPanel.cpp:522`: metadata v1, optional
  language trailer, legacy metadata remains valid.
- `../matcha-reader/tools/manga_convert/convert_manga.py:901`: rewrites index then
  data after each page; this is not atomic and is not checkpoint-based resumption.
- `src/activities/reader/ReaderActivity.cpp:211`: existing format dispatch.
- `src/activities/ActivityManager.cpp:554`: reader entry factory.

Malformed input must fail without exposing partially decoded page state. Optional
metadata/TOC failure must allow the future reader to fall back to folder name and
percentage navigation. Reserved bytes are ignored for compatibility. Trailing page
bytes are tolerated as upstream does. Page dimensions and OCR rectangles are preserved,
not silently rescaled or clamped by the decoder. Reject unsupported panel versions.

## Remaining integration stages

1. MangaBook HAL adapter: folder-marker detection, canonical page paths, natural-sort
   fallback excluding panel crops and hidden files, both crop directories, metadata,
   optional TOC, panel-only and missing-image handling.
2. Reader activity: tested page/panel navigation state, Matcha button behavior, touch
   adaptation, per-book panel/rotation settings, progress, chapters and bookmarks.
3. Library: browser folder recognition, Home/Recents/resume, cover thumbnails, actions,
   reading statistics and finished state using CrossInk stores.
4. Images and disposable cache: common fit/rotation geometry, bounded decode, grayscale,
   fresh/cache parity, source identity and partial-file rejection, no second framebuffer.
5. Prefetch: bounded idle jobs, cancellation/join on exit, temporary-file publication,
   no renderer mutation in worker, foreground priority and same-file coordination.
6. OCR: adapt panel text/coordinates to existing PageTextSource/PageWordScanner and
   DictionaryLookupFlow; preserve both dictionaries, font restoration, saved words,
   nested lookup, and scan identity against source plus dictionary changes.

Detailed source audit accompanies the task plan. None of these later stages is
implicitly completed by passing format tests.

## Audit decisions for implementation

The [reader audit](../plans/2026-09-05-manga-reader-audit.md) records the precise
dependency map and menu inventory. Follow pinned Confirm-to-menu behavior in both
overview and panel mode; lookup is a menu command. Implement per-book rotation as
the handoff requests, documenting that upstream's setting is global. Debounce
progress rather than copying upstream's write-on-every-render behavior.

Do not reuse upstream pixel/scan cache identity unchanged: pixel headers lack source
and settings identity, and upstream scan identity misses same-size dictionary changes.
Retain CrossInk's stronger dictionary signature. JPEG worker cancellation currently
ignores requests after two-thirds completion; remove that exception in the future
cache-only worker path before claiming bounded exit latency.

The converter has an OCR rectangle defect at upstream lines 1507–1516: it offsets
normalized boxes by the unmargined panel origin and writes endpoints where the binary
writer expects width/height. Correct newly produced rectangles with an offline
regression test; preserve the format and read existing books without rewriting them.
The cost of this adaptation is a small, explicitly documented difference from pinned
converter output for OCR rectangles; reproducing the defect would misplace lookup
highlights. Golden compatibility fixtures still come from the original writer.

## Verification and hardware handoff

Use deterministic original-writer fixtures, malformed/truncated cases, unaligned
buffers, legacy metadata, UTF-8 OCR/translation and TOC. Converter tests must be offline;
no cloud OCR, credentials, paid requests, or automatic model downloads. Run native
CTest sequentially; preserve dictionary and font regressions. Build firmware profiles
when their integration changes land and watch the last reported 50288-byte X4 OTA
headroom. Simulator cannot establish image parity.

Physical acceptance: X4/C3 and Sticky/X4 Pro/S3, open nested manga, traverse panels
forward/backward, rotate, bookmark/reopen/sleep, compare cache and uncached output,
rapidly cancel prefetch, check ghosting and internal free/max-allocation heap. Preserve
user fonts. Safely restore `/dictionaries/jp.user-backup-20260905` during the hardware
handoff after checking both dictionary paths; restoration and outstanding dictionary
hardware parity remain pending, not claimed complete.
