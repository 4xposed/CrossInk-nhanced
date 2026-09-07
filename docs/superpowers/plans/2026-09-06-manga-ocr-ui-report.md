# Task 9b: shared manga lookup and stored translations

Task 9b implemented and software verification complete on `matcha_features`; no commits, staging, pushes, dictionary/font-engine/SDK edits, PIO commands or serial access by this worker. Parent owns firmware builds and simulator runs. Baseline: `/private/tmp/crossink-manga-ocr9b-before`. Existing unrelated changes preserved.

Final evidence: button OCR smoke **passed (28.742 s)**, touch OCR smoke **passed (28.997 s)**, existing EPUB smoke **passed (3.360 s)**, and full native suite **659/659 passed (31.39 s)**. Scoped independent review reported no production blockers. External scan caching remains disabled by design for Task 9c; physical device/SD-font/e-ink validation remains unperformed because no USB reader is enumerated. Later sections retain fixture-failure diagnoses so their corrections are reviewable; this paragraph is the final status.

## Implementation

- `src/activities/reader/EpubLookupRequest.h`: optional owned `scanCacheFilePath` and synchronous `renderExternalBackground(void*, PageTextSourceView)` callback. Its view expires on callback return.
- `src/activities/reader/EpubReaderWordLookupActivity.{h,cpp}`: third constructor moves `OwnedLookupTextSource`; both prior constructors remain intact. `sourceView()` and `sourceTruncated()` route scanner, candidate encoding, highlight/touch, clipping and cache identity accesses. External entry never builds/reloads an EPUB Page and retains its glyph owner across dictionary switches. `releaseOwnedState()` clears the external callback and owner only after the existing worker/scanner shutdown. Existing definition worker, scanner, dictionary routing, suggestions, nested selection/back, history, clipping result, and font activation/restoration remain shared.
- External persistent scan load and save are explicitly disabled until Task 9c supplies verified scan identity. Per-source path, physical page, panel+1, hash and glyph count are prepared without trusting sampled signatures. Truncated sources visibly display “Lookup (partial text)”; truncated-source cache loading is also prevented.
- `src/activities/reader/MangaReaderActivity.{h,cpp}`: appends Lookup, Translation and Lookup History to existing menu indices. Latest parent steering restores pinned handoff behavior: Confirm in a panel queues direct lookup; overview Confirm and touch menu gesture open the menu. Repeated pending Confirm edges coalesce until prefetch is idle; child construction occurs outside the reader RenderLock. Lookup snapshots physical scope, builds selected-panel or overview OCR, and passes manga cache/language/global dictionary font settings to the existing activity. Parent retains immutable page bytes for clipping reconstruction after child source teardown.
- The locked background callback draws BW only, temporarily selects overview and restores the original `position` and renderer orientation before returning. It never invokes grayscale display/refresh. Missing overview or unusable metadata uses fixed-cell OCR text above the definition panel; every glyph draw uses the renderer's existing text clip. The callback receives a live view synchronously, so the parent never retains a glyph pointer.
- Clipping uses `copyMangaLookupClipping`, followed by `ClippingsManager::saveClipping` with bounded book/author labels, one-based physical page and panel/overview context. No EPUB Section/clipping store APIs are called. Save/failure feedback is translated.
- `src/activities/reader/MangaTranslationPager.h`: allocation-free paging directly over immutable stored translations, independent of OCR and dictionaries. Each page rewalks bounded metadata (maximum 32 KiB); no concatenated translation copy or unbounded page-offset vector. UTF-8 glyphs remain whole, malformed bytes become visible `?`, controls become spaces, and CRLF/newline/panel boundaries wrap rows. Scope and geometry errors are explicit.
- `MangaTranslationActivity.{h,cpp}`: renders clipped fixed cells with reader font, page buttons/swipes/taps, header Back and translated empty/error states. Only one e-ink framebuffer is used.
- English YAML source and normal `scripts/gen_i18n.py` generation supply strings; the Task 9a placeholder in `CHANGELOG.md` is consolidated into a user-facing menu feature entry.

## Memory and lifetime

Task 9a's reviewed 1024-glyph/256-glyph allocation fallback remains unchanged. This integration moves that owner rather than copying it. Clipping allocates at most 4097 bytes temporarily (1024 four-byte UTF-8 codepoints plus terminator), using `makeUniqueNoThrow`; this exceeds the small C3 stack budget and cannot be static across activities. The existing string-based clipping API receives one bounded cold-path string. Translation uses borrowed metadata and a small stack drawing context. No definition/dictionary/font implementation is copied or replaced.

Prefetch is drained by menu entry, remains parked while menu is active, and the existing `prepareToSuspend` gate protects child transitions. Lookup additionally checks idle before source construction. Parent navigation cannot reload the borrowed page while the child is active. Normal return marks the image dirty and resumes statistics; forced teardown drains the lookup first and releases the manga parent afterward.

## Native verification

```sh
cmake -S test -B /private/tmp/crossink-manga-ocr9b-test \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/private/tmp/crossink-manga-pixel-build/_deps/googletest-src \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build /private/tmp/crossink-manga-ocr9b-test --target JapaneseDictionaryTest MangaTranslationTest -j 2
/private/tmp/crossink-manga-ocr9b-test/japanese_dictionary/JapaneseDictionaryTest
/private/tmp/crossink-manga-ocr9b-test/manga_format/MangaTranslationTest
python3 scripts/gen_i18n.py
python3 -m py_compile scripts/run_manga_simulator_smoke_test.py
git diff --check
```

286 dictionary tests pass, including the existing EPUB, Japanese, StarDict and 16 manga adapter tests. Five translation tests pass: selected/overview scope, no OCR, previous-page revisiting, four-byte UTF-8, malformed/control/CRLF handling, empty/invalid input, exact page boundaries and 12,000-byte translation paging. Logs: `/private/tmp/crossink-manga-ocr9b-dictionary.log`, `...-translation.log`, `...-native-build.log`. Initial test compilation failed because the pager did not yet exist; initial fixture serialization was then corrected to the actual panel header layout before the behavioral tests passed.

Parent reported initial simulator build success. The opt-in `CROSSINK_SIMULATOR_MANGA_OCR=1` fixture extends the existing manga smoke without changing its default gate; it installs local English OCR and long stored translation using the existing real StarDict fixture. Full runtime results and remaining test coverage will be appended after the parent completes runs.

## Limits and hardware verification

External persistent scan reuse is intentionally not implemented until Task 9c. Task 10c must preserve dictionary routing/history during disposable manga cache cleanup/move; the pre-existing manga full-cache deletion path still needs that separate integration.

On X3/X4, Sticky and X4 Pro: use overview and legacy crop books, all orientations, missing-overview fallback, Japanese and StarDict, reader/dictionary SD fonts, repeated cursor/definition/nested/switch/back cycles, clipping and history reopen. Confirm exact prior panel/image/orientation after normal and forced child exit. Verify whole-block highlights align with BW overview, no grayscale pass paints over the lookup panel, partial-source warning appears under cap/OOM, and no external scan cache is published. Test translation-only and no-dictionary books offline. Record internal heap/largest block and worker stack watermarks; simulator does not certify real SD single-reader behavior, e-ink ghosting or SD-font memory pressure. No performance improvement is claimed; no source/image cache reset is required by this UI change.

## First simulator failure and fixture correction

The first OCR smoke reached the actual Reader definition in 6 ms but then failed its wait for candidate 1. The original generic smoke fixture sorted mixed-case words using Python byte order: Alignment, Reader, This, paragraph, text, the. StarDict searches case-insensitively (`src/util/Dictionary.cpp:390`, `:901`) and stops once index order passes its target (`:922`); therefore “This” stops a search before reaching “text”.

A native regression reproduces the exact corpus and proves both causes independently: the real manga adapter/scanner yields candidate 1 at glyph 7, count 4, source ordinal 1, encoding exactly “text”; Reader lookup succeeds and text lookup returns NotFound. Reordering only the same six index records, keeping definition offsets and bytes unchanged, makes text lookup succeed. Test: `MangaSmokeFixtureNeedsStarDictCaseInsensitiveIndexOrdering` in the JapaneseDictionaryTest fixture. This is a fixture correction; no dictionary algorithm changed and the pre-existing generic EPUB smoke corpus remains untouched.

The OCR-specific fixture now uses a correctly ordered Reader/text dictionary with a long “text” definition and a nested Reader reference, plus the existing second local dictionary for actual switching. Expanded script assertions cover selected query/glyph ordinal diagnostics, definition page 1, nested lookup and Back restoration to page 1, dictionary selection/reinitialization, exact clipping, stored history before disposable cache-clear tests, translation page 1/back to page 0, and reader font ID/width/line-height on return.

Selected reader SD-font loading uses the existing `sdFontSystem.ensureLoaded` API before source fallback geometry and translation entry, since earlier network flows may release the active reader font. The font subsystem itself remains unchanged. Real SD-font pressure/activation still requires hardware validation.

## Expanded acceptance batch

Parent verified the preceding source checkpoint: OCR smoke passed in 25.663 s, EPUB smoke passed in 3.314 s, and the full native suite passed all 659 tests in 31.39 s. Logs are `/private/tmp/crossink-manga-ocr9b-smoke.log`, `...-epub-smoke.log`, and `...-native-tests.log`.

The final fixture batch adds a translation-only second page, a separate empty-text book, and a legacy crop-only book without overview assets. Real panel Confirm exercises the no-OCR feedback. Translation is checked with no OCR, empty text and unavailable dictionaries. The no-dictionary check temporarily renames only the runner's generated `/dictionaries` fixture directory while no dictionary activity exists, then restores it; it never addresses user storage or actual installed dictionaries. Normal empty OCR now logs informationally; malformed/OOM failures remain errors.

Repeated real Confirm edges are sent while the existing prefetch completion-consumption barrier is held; the reader must stay foreground until worker cleanup is acknowledged, then open a single shared lookup. The script forces global replacement of a live lookup, verifies the existing forced teardown path, reopens the manga book and checks the original panel and reader font metrics. Crop-only lookup verifies the fixed-cell fallback through the real shared engine. Touch-capable runs use actual source-area horizontal swipes for word movement, a translation next-page tap and board-specific menu swipes; button runs retain the button equivalents.

Final command (runner is headless by default, no `--headless` flag):

```sh
CROSSINK_SIMULATOR_MANGA_OCR=1 python3 scripts/run_manga_simulator_smoke_test.py --no-build --env simulator --timeout 90
CROSSINK_SIMULATOR_MANGA_OCR=1 python3 scripts/run_manga_simulator_smoke_test.py --no-build --env sticky-simulator --timeout 90
```

The test harness uses read-only simulator accessors for actual lookup state/query/candidate, translation page/empty state and reader position/font metrics; it does not bypass lookup/navigation to force successful definition results.

## Exact Task 9b file set

Modified existing/shared files: `src/activities/reader/EpubLookupRequest.h`, `src/activities/reader/EpubReaderWordLookupActivity.h`, `src/activities/reader/EpubReaderWordLookupActivity.cpp`, `src/activities/reader/MangaReaderActivity.h`, `src/activities/reader/MangaReaderActivity.cpp`, `src/simulator/SimulatorSmokeTest.cpp`, `scripts/run_manga_simulator_smoke_test.py`, `test/manga_format/CMakeLists.txt`, `test/japanese_dictionary/JapaneseDictionaryTest.cpp`, `lib/I18n/translations/english.yaml`, and `CHANGELOG.md`.

New files: `src/activities/reader/MangaTranslationPager.h`, `src/activities/reader/MangaTranslationActivity.h`, `src/activities/reader/MangaTranslationActivity.cpp`, `test/manga_format/MangaTranslationTest.cpp`, and this report. Normal I18n generation refreshed ignored/generated artifacts. Existing manga files were untracked before this task and remain untracked. Task 9a source adapter files, dictionary implementations, fonts and SDK have not been edited.

Final software evidence is recorded at the top and below. Actual C3/S3 hardware, SD font swap/memory pressure, physical e-ink overlays and forced teardown during real SD I/O remain unverified because the parent reports no enumerated USB reader. External persistent cache is intentionally disabled pending Task 9c, as required by this brief.

The first expanded button run passed the OCR/translation/empty/no-dictionary/prefetch/forced-exit phases, then failed the pre-existing file-browser bookmark-reopen gate. Its cache hash was 1843028987 (`/books/smoke-manga-crop-only`) instead of 4225682694 (`/books/smoke-manga`): adding auxiliary books under `/books` changed the browser's selected entry. The fixture-only fix moves auxiliary books outside that directory to `/manga-ocr-fixtures/{empty,crop-only}`, preserving the baseline browser input contract. No reader bookmark/progress code was changed.

The same final test patch strengthens normal translation-page checks to require nonempty content, requires exact no-OCR page/panel log evidence, and records reader orientation plus the actual initial panel framebuffer hash. Parent-return assertions now render and settle before comparing the main book's same-panel image hash. This verifies the displayed image, not only the saved numeric position. These changes touch only `src/simulator/SimulatorSmokeTest.cpp` and `scripts/run_manga_simulator_smoke_test.py` (plus this report).

The first touch run correctly moved to the second real definition, then treated the return swipe as Back. The fixture started the rightward swipe at exactly one quarter of the screen; `MappedInputManager.cpp:24` and `:600` define the leftmost 25% (inclusive) as the Back gesture zone. The fixture now starts at the midpoint. No input or shared lookup behavior was changed.

Independent review requested stronger empty-message and clipping assertions. A SIMULATOR-only `OptionPopup::simulatorTitleIs(const char*)` compares the actual displayed title without exposing a borrowed string; `MangaReaderActivity::simulatorNoOcrFeedback()` compares it with `tr(STR_MANGA_NO_OCR)` under RenderLock. The runner parses the clipping record and asserts the selected-text field is exactly `Reader`, independently of its metadata/date, and separately validates physical page/panel context. Add `src/components/OptionPopup.h` to the exact touched-file list; its change is test-only and contributes no firmware behavior or storage.

Final corrected button fixture passes in 28.742 s; unchanged EPUB smoke passes in 3.360 s. Parent reports the scoped independent review found no production blockers and all requested assertion improvements addressed. The touch matrix is still running at this checkpoint. Firmware source remains held for attributable verification.


## Final handoff

Task 9b is done. Parent confirmed the corrected touch fixture passed in **28.997 s**, following the final button and EPUB passes above. Final logs: `/private/tmp/crossink-manga-ocr9b-smoke.log`, `/private/tmp/crossink-manga-ocr9b-touch-smoke.log`, `/private/tmp/crossink-manga-ocr9b-epub-smoke.log`, and `/private/tmp/crossink-manga-ocr9b-native-tests.log`. `git diff --check` passes and final `git status --short` was inspected; no staging/commits/pushes performed. All firmware source is held for the parent's final review and Task 9c integration.

The crop-only fixture intentionally has no overview/cover; its background uses OCR fixed cells successfully, while library cover attempts log the existing `MCV` missing-cover state. Other missing-sidecar/font-directory simulator messages are expected fixture behavior. No physical hardware performance, SD-font stress result or persistent external scan correctness is claimed.
