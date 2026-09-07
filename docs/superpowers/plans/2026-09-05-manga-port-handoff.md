# Matcha manga port — feature checklist and fresh-session handoff

## User direction

Port Matcha's complete manga implementation into CrossInk: the same custom indexed
format, converter, reader, caching, thumbnails, prefetching, library integration,
OCR text, and dictionary lookup. Preserve exact compatibility with existing
Matcha-converted manga. Coexist with EPUB, TXT, XTC, Anki, OPDS, and both dictionary
backends. The user explicitly requested starting manga after the dictionary work
and now wants a fresh session to do it.

This is an inventory and handoff, not a completed implementation design. The next
session should audit the pinned implementation, produce concrete integration
tasks, and proceed under the user's existing authorization. Do not restart the
earlier choice between porting CrossInk into Matcha and porting Matcha into CrossInk.

## Workspace and safety

- Work in `/Users/daniel/code/CrossInk`, on the user's `matcha_features` branch.
- Read `AGENTS.md` and `.claude/CONTEXT.md`. Never commit, amend, push, or discard
  existing changes. During this handoff the dictionary changes were committed
  externally as `ea039400` (`add a mini Yomitan`); this agent did not commit them.
  Final status showed only `.codegraph/` and `docs/superpowers/` untracked. Recheck
  status before starting; the design/plan/handoff documents must be preserved.
- Reference source: `/Users/daniel/code/matcha-reader`, pinned at
  `61ca61ba86e3c5709a24d1b9c4f3cf2d41488012`. Inspect local changes before treating
  working files as identical to that revision.
- CrossInk SDK currently points to `1e8ee543edca397f2b8747811f5a88f1bc35d233`.
  Recheck submodule status before SDK work; prefer CrossInk HAL boundaries.
- Both repos have `.codegraph/`. Use CodeGraph first. Matcha's `codegraph explore`
  failed during this handoff with “unable to open database file”; targeted reads
  were used as fallback. Do not rebuild its index without user direction.

## Features to deliver

### 1. Exact indexed manga format

- [ ] Recognize manga folders by `panels.idx`, regardless of folder name or depth.
- [ ] Read `panels.idx` and `panels.dat` with the pinned reader's supported
  versions, byte order, field widths, offsets, lengths, and optional fields.
- [ ] Preserve page dimensions, panel rectangles/order, OCR text rectangles,
  UTF-8 text, and per-panel translation strings.
- [ ] Read `meta.bin`: title, author, optional language trailer, legacy files
  without language, and sensible missing-metadata fallbacks.
- [ ] Read optional `toc.idx`; preserve chapter labels and page indexes.
- [ ] Support canonical `page_NNNN` images and the pinned reader's fallback
  discovery/order rules for older layouts.
- [ ] Support panel crops in `panels/p<page>_<panel>.jpg` and older flat folders.
- [ ] Handle panel-only books, pages without detected panels, missing images,
  and malformed/truncated indexes without crashes or excessive allocation.
- [ ] Document the binary contract and test with files from the original
  converter. Do not infer a version from prose: the converter docstring says
  version 1 while `MangaPanel.h` declares version 2 with translations.

### 2. Host converter

- [ ] Port the pinned converter and its dependency/setup documentation.
- [ ] Preserve input support: image folders, CBZ, ZIP, EPUB, PDF; verify actual
  code paths and optional dependency requirements for each.
- [ ] Preserve source ordering, explicit page-order files, metadata extraction,
  title/author/language overrides, and chapter extraction where supported.
- [ ] Preserve panel detection, Japanese reading order, YOLO detection and
  white-gutter fallback, crop margins, sizing, and output naming.
- [ ] Preserve X3/X4 sizing profiles, monochrome/dithered BMP option, image
  preprocessing and encoding behavior, page limit, and no-OCR mode.
- [ ] Preserve OCR and pre-extracted translation output, bounding boxes and
  coordinate transformations after resizing/cropping.
- [ ] Audit checkpoint/resume behavior and per-page index rewriting; retain
  recoverability and avoid claiming resumability beyond source behavior.
- [ ] Keep OCR optional. Cloud OCR sends panel images to Gemini and requires
  user-provided credentials; do not send user pages or incur charges merely to
  test the port. Use offline fixtures/mocks for automated testing.
- [ ] Existing Matcha browser-tool output must load. Building a separate browser
  converter is not established scope; record any need discovered during audit.

### 3. Reader and navigation

- [ ] Full-page overview and ordered panel zoom; advance through panels/pages
  and reverse correctly across boundaries.
- [ ] Matcha's button semantics: page-turn enters/advances panel zoom, Confirm
  opens the overview menu or panel text lookup, Back leaves panel zoom or book,
  and held Back returns to file browsing.
- [ ] Per-book “Panels Only” and “Rotate Panels” preferences; portrait/landscape
  geometry, upright fitting when rotation is disabled, and correct overlay
  coordinates after rotation.
- [ ] Automatic panel mode when overview images are absent; overview fallback
  for a page with no panels.
- [ ] Chapter selection with percentage-jump fallback, bookmarks and bookmark
  navigation, saved page/panel position, reopen/resume, and sleep/wake behavior.
- [ ] Metadata/status/progress presentation and pre-extracted panel translation
  display as supported upstream. Inventory actual menu commands before porting.
- [ ] Integrate through ActivityManager/RenderLock, MappedInputManager, UITheme,
  and translated strings. Adapt touch-device input without changing the file
  format or weakening button-device support.
- [ ] Keep ordinary word/panel navigation on fast refreshes; retain an explicit
  cleanup policy for e-ink ghosting and verify it on hardware.

### 4. Image rendering and caches

- [ ] Port required page/panel image decode and pixel-cache paths, including
  grayscale and monochrome behavior, aspect fit, rotation, and crop geometry.
- [ ] Cached pixels and fresh decoding must produce matching geometry/output.
- [ ] Recover geometry from a valid cache without opening the source image just
  to read dimensions; preserve canonical page-list fast paths.
- [ ] Define cache identity/invalidation for source replacement, geometry,
  rotation/settings, versions, and corrupt/partial files. Audit Matcha's exact
  cache contract and distinguish it from the interoperable book format.
- [ ] Caches remain disposable; failures fall back safely to source rendering.
- [ ] Do not add a second framebuffer. Keep C3 memory bounded; justify owned
  buffers, handle allocation failure, and capability-gate S3 enhancements.

### 5. Prefetching

- [ ] Idle prefetch of the next full page and first/next panel where useful.
- [ ] Shared geometry rules between foreground rendering and cache warming.
- [ ] Background worker must not mutate the renderer or activity stack.
- [ ] Bounded job/result ownership; cancellation on navigation/exit, safe task
  shutdown, and no speculative work blocking foreground input.
- [ ] Temporary-file publication/rename and safe coordination when foreground
  and worker request the same asset; respect real SD access constraints.
- [ ] No cache corruption, leaked tasks/handles, or monotonic heap degradation
  during rapid navigation, repeated opens, cancellation, and sleep/resume.

### 6. Thumbnails, library, and persistence

- [ ] Discover manga alongside existing books in CrossInk's browser/library,
  shelves/folders where applicable, Home, Recent Books, and Continue Reading.
- [ ] Show Japanese title/author, cover, progress, and supported book actions.
- [ ] Generate/reuse size-appropriate 1-bit BMP thumbnails; prefer first page
  images and never accidentally select a panel crop as the cover.
- [ ] Handle cover generation cancellation/OOM and missing or damaged covers.
- [ ] Store progress/bookmarks/settings through existing app stores where
  appropriate. Integrate reading time, language attribution, and finished-book
  behavior; do not replace CrossInk's entire library/theme system for parity.
- [ ] Audit move/rename/delete/cache-clean actions so manga folders and their
  durable user state behave consistently with other supported books.

### 7. OCR text and dictionary lookup

- [ ] Expose ordered panel OCR text and its coordinates for selection/lookup.
- [ ] Reuse the unified Japanese/StarDict engine and definition model already
  ported; keep names, grammar, segmentation, longest matches, and deinflection.
- [ ] Progressive word discovery, navigation, definition paging, cancellation,
  empty OCR/no match/error states, and dictionary/reader font restoration.
- [ ] Reusable scan results keyed by page/panel and validated against OCR text
  and dictionary identity; no stale results after source/dictionary changes.
- [ ] Preserve supported lookup history, saved words/clippings, and nested
  lookup behavior where applicable. Audit upstream MangaWordLookupActivity
  separately: do not assume it has the same UI contract as EPUB lookup.
- [ ] No live OCR or cloud translation dependency for on-device reading.

## Source entry points verified during handoff

- `../matcha-reader/lib/MangaPanel/MangaPanel.h:13`: format version; `:43` onward:
  MangaBook, index/image discovery, metadata, TOC, cover thumbnails.
- `../matcha-reader/lib/MangaPanel/MangaPanel.cpp`: binary parsing and discovery.
- `../matcha-reader/tools/manga_convert/convert_manga.py:40`: folder layout;
  `:69`: binary-format prose; `:901`: index writer; `:916`: TOC writer;
  `:1277`: CLI flags. Verify constants/writers rather than trusting comments.
- `../matcha-reader/src/activities/reader/MangaReaderActivity.{h,cpp}`:
  reader, geometry, caches, worker lifecycle; header `:184` documents prefetch.
- `../matcha-reader/src/activities/reader/MangaWordLookupActivity.{h,cpp}`:
  OCR lookup and persisted scans.
- `../matcha-reader/src/activities/reader/MangaBookmarksActivity.*` and
  `MangaChapterSelectionActivity.*`: navigation screens.
- `../matcha-reader/lib/Epub/Epub/converters/PixelCache.{h,cpp}`:
  shared pixel-cache dependency to audit before copying.
- `../matcha-reader/USER_GUIDE.md:730` and `README.md:139`: user behavior/converter.

## Suggested implementation order and acceptance

1. Inventory pinned dependencies/licenses and map them to existing CrossInk
   format dispatch, stores, UI, renderer, and dictionary interfaces.
2. Specify binary compatibility and establish original-converter fixtures.
3. Port format reader/converter; test round trips and malformed files.
4. Deliver a usable page/panel reader with progress/settings/bookmarks/TOC.
5. Integrate library discovery and thumbnails.
6. Add pixel caches and cancellable prefetch, with explicit C3 memory checks.
7. Connect OCR selection to the unified lookup engine and scan persistence.
8. Verify all features as one workflow on C3 and S3 hardware, documenting any
   remaining limitations before declaring full parity.

Use deterministic small fixtures: multiple pages/panels, Japanese OCR, translation,
TOC, non-ASCII metadata, both crop layouts, no-OCR, panel-only, zero-panel pages,
and malformed variants. Test cached/uncached image equivalence and interruption.
Build `default`, `sticky`, `x4-pro`, and suitable simulator profiles. The simulator
has known image-decoder limitations, so a passing simulator cannot prove image
parity. Perform physical image/rotation/ghosting/prefetch/heap validation.

## Dictionary handoff — preserve these facts

- Existing spec/plan: `docs/superpowers/specs/2026-09-04-unified-japanese-dictionary-port-design.md`
  and `docs/superpowers/plans/2026-09-04-unified-japanese-dictionary-port.md`.
- Dictionary implementation and automated review were completed previously.
  Latest native run: **504/504 passed** using `build/task16-tests`, sequential
  CTest (`-j 1`). Parallel tests collide in shared temporary fixture directories.
- Latest X4 `default` build and upload succeeded: RAM 62,068 bytes; firmware
  image 6,503,312 bytes; OTA headroom 50,288 bytes. Manga flash growth needs care.
  Other board builds passed earlier, before the latest font/refresh fixes.
- Hardware: Xteink X4 / ESP32-C3, no PSRAM, `/dev/cu.usbmodem1101` at last check.
  User confirmed Japanese titles/body now render correctly and lookup opens.
- Font fix: EpdFont::findGlyph now invokes the SD miss callback for uncached
  glyphs. EpdFontFamily::hasCodepoint includes regular-face style fallback.
  Preserve both and their regression tests.
- Lookup opening now uses fast refresh even from the menu, and suppresses brief
  loading frames. Latest measured first definitions: 255 and 283 ms. The user
  accepted occasional cleanup flashing: HALF_REFRESH every 10 redraws.
- Full dictionary hardware parity was **not** exhaustively certified: S3 tests,
  long-run heap/navigation, missing/replaced dictionaries, and the remaining
  original hardware matrix still need recording. User has now directed manga
  work to begin; do not claim these outstanding checks passed.

### Temporary SD fixtures still require cleanup

The user's original Japanese dictionary is backed up on the device as
`/dictionaries/jp.user-backup-20260905`. The active `/dictionaries/jp` is a tiny
test dictionary. Restore the backup safely after verifying both paths. Never
overwrite/delete the backup. Other test artifacts:

- `/dictionaries/en/crossink-hardware-smoke`
- `/CrossInk-Japanese-Dictionary-Hardware-Fixture.epub`
- `/CrossInk-StarDict-Hardware-Fixture.epub`
- Fixture cache: `/.crosspoint/epub_6071926278859914744/`

Existing `/fonts/NotoSansJP` and `/fonts/BookerlyJP` belong to the user; preserve.
Temporary host tools: `/private/tmp/crossink_serial.py`, `crossink_transfer.py`,
`crossink_capture.py`. Python with pyserial:
`/opt/homebrew/Cellar/platformio/6.1.19_2/libexec/bin/python`.
Serial monitor session at handoff: **72547** (may no longer exist in a fresh session).
Release any monitor before flashing; do not run concurrent commands on one port.
USB transfer requires the device's main Home screen; internal dot paths are
protected. A prior broad settings-file read was rejected by automatic approval
review; do not bypass that restriction or read credentials for diagnostics.

## Fresh-session starter

Read `AGENTS.md`, `.claude/CONTEXT.md`, and this handoff. Continue the authorized
Matcha manga port on `matcha_features`, preserving the dictionary work. Start with
the pinned source audit and a concrete integration design/task plan covering the
checklist; then implement and validate. Do not commit or push. Track the remaining
dictionary hardware checks and restore the user's temporary SD dictionary backup
as part of the hardware handoff, without treating those checks as completed.
