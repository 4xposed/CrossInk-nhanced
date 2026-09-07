# Task 9b review — shared manga lookup and translation

## Spec Compliance

- ✅ The reviewed implementation matches Task 9b: an owned external entry preserves the two existing lookup constructors and shared engine (`EpubReaderWordLookupActivity.cpp:177`, `:189`); common source access replaces EPUB-only accesses (`EpubReaderWordLookupActivity.h:161`); external persistent scans are disabled pending verified identity (`EpubReaderWordLookupActivity.cpp:318`, `:423`); manga menu/direct Confirm, translation and parent clipping are integrated (`MangaReaderActivity.cpp:239`, `:423`, `:515`, `:581`).
- ✅ Final software acceptance passes: corrected button OCR smoke at 28.742 seconds, touch OCR smoke at 28.997 seconds, and EPUB smoke at 3.360 seconds. Reviewer read the success markers and runner validation in `/private/tmp/crossink-manga-ocr9b-smoke.log`, `/private/tmp/crossink-manga-ocr9b-touch-smoke.log` and `/private/tmp/crossink-manga-ocr9b-epub-smoke.log`. The controller reports the prior 659 native passes remain applicable because the follow-up changes do not modify native production/test targets.
- ⚠️ Physical C3/S3 SD contention, dictionary/reader SD-font pressure, all-orientation overlay alignment and e-ink refresh quality remain hardware verification items. No hardware run is claimed. External persistent cache verification belongs to Task 9c; cache-route/menu/status/shortcut preservation assigned to Tasks 10c/10e is outside this gate.

## Strengths

- External glyph storage remains owned by the lookup through dictionary changes; rescan clears scanner/cache state without releasing the external owner (`EpubReaderWordLookupActivity.cpp:1181`). Normal and forced teardown cancel/join the worker before clearing source storage and restore the reader font (`:470`, `:494`, `:439`).
- The background callback accepts a synchronous source view, draws BW without the manga grayscale refresh sequence, and restores temporary overview selection and renderer orientation before returning (`MangaReaderActivity.cpp:375`). No parent-held glyph pointer survives child release.
- The parent snapshots clipping scope and reconstructs bounded UTF-8 from immutable page data after the lookup exits, using `ClippingsManager` with physical page/panel context (`MangaReaderActivity.cpp:501`). The temporary 4097-byte heap buffer is bounded and justified against C3 stack limits.
- Translation traverses stored metadata directly without OCR or dictionary dependencies and without an offset vector/concatenated text allocation (`MangaTranslationPager.h:12`). Native cases verify real UTF-8 output, selected/overview scopes, malformed input, revisit behavior and exact boundaries (`MangaTranslationTest.cpp:28`).
- Panel Confirm is retained as one pending intent until prefetch is idle, and dispatch occurs outside the parent RenderLock (`MangaReaderActivity.cpp:581`, `:636`). Existing dictionary worker/definition/navigation implementations are reused.

## Issues

### Critical

- None found in the reviewed snapshot.

### Important

- None remaining. `run_manga_simulator_smoke_test.py:157` and matching `MangaOpenFixture` actions isolate auxiliary books from the retained browser regression; `SimulatorSmokeTest.cpp:750` avoids the intentional Back zone. Corrected button/touch/EPUB runs pass. No production bookmark or touch-navigation defect was identified.

### Minor

- No assertion findings remain from the initial review. Fix1 checks nonempty translation (`SimulatorSmokeTest.cpp:1322`), the actual translated empty-OCR popup title through a SIMULATOR-only read-only accessor (`MangaReaderActivity.cpp:1086`, `OptionPopup.h:108`), and exactly one clipping body equal to `Reader` with physical page/panel context (`run_manga_simulator_smoke_test.py:293`).
- **Diagnostic noise, nonblocking:** `/private/tmp/crossink-manga-ocr9b-smoke.log:566` (also `:583`, `:590`, `:604`) contains `[ERR] [MCV] Manga cover page is missing`; missing-cache open messages also remain in this fixture run. The runner succeeds, but its output is not pristine. Explicitly account for expected fixture diagnostics when reporting the passing gate; no unrelated cover implementation change is requested by this review.

## Focused dependency checks

- **Parent/source lifetime and result-lock risk:** CodeGraph first inspected `ActivityManager::loop`; the returned block ended mid-function, so only its missing continuation (`ActivityManager.cpp:252–332`) was read directly. Pop destroys the child before restoring the parent and invokes the result callback after unlocking (`:236`, `:267`); replacement exits current child before parked parents (`:293`).
- **Background orientation, source-reader overlap and prefetch restart risk:** CodeGraph inspected `MangaReaderActivity::drawImageLocked`, `foregroundReadyLocked`, `pollPrefetchLocked` and `warmLocked`. Drawing restores base orientation (`MangaReaderActivity.cpp:934`); foreground entry cancels/drains prefetch (`:1039`); menu/suspended/no-render-time states prevent speculative restart (`:1053`).
- **Worker shutdown and dictionary-rescan lifetime risk:** CodeGraph inspected `EpubReaderWordLookupActivity::shutdownBeforeFinish`, `onExit`, `initializePageMode` and `openDictionarySwitcher`. The source owner survives rescan and is released after worker join, with render disabled first (`EpubReaderWordLookupActivity.cpp:475`, `:485`, `:451`, `:1181`).
- **Failed metadata read and clipping borrow risk:** CodeGraph inspected `MangaBook::readPageInfo`, its `PageView` lifetime contract, and `copyMangaLookupClipping`. `readPageInfo` zeroes output before failure (`MangaBook.cpp:134`); only load/open/close invalidate page bytes (`MangaBook.h:9`), not background image-path/read-info calls.
- Review used `/private/tmp/crossink-manga-ocr9b-review.patch` plus its supplemental CMake/dictionary-fixture source. No source edits, test runs, PIO/serial commands, SDK/font/dictionary implementation changes, subagents or Git mutations were performed; only this report was written.
- Scoped follow-up reviewed only `/private/tmp/crossink-manga-ocr9b-fix1-review.patch` and the unresolved findings. Added settled-render framebuffer hashing and orientation comparisons strengthen return-to-image evidence without modifying production rendering (`SimulatorSmokeTest.cpp:1151`, `:1349`). No new code-quality issue was found in this delta.

## Assessment

**Task quality: Approved.**

No production correctness blocker was found by the scoped code review, fix1 resolves the initial assertion-strength findings, and final button/touch/EPUB gates pass. Actual-device storage contention, large SD-font pressure and physical e-ink overlays remain explicitly unverified hardware limits, separate from this software approval.
