# Manga Port Implementation Plan

> **For agentic workers:** Use executing-plans or subagent-driven-development task by task. Do not commit or push.

**Goal:** Begin the authorized complete Matcha manga port with a verified binary/converter foundation, then integrate it through existing CrossInk boundaries.

**Architecture:** Allocation-free binary views over caller-owned buffers; a HAL-backed MangaBook adapter; foreground reader and bounded cache worker; reuse CrossInk library and dictionary services.

**Tech Stack:** C++20, HAL storage, PlatformIO, native CMake/GTest, Python unittest.

**Spec:** `docs/superpowers/specs/2026-09-05-manga-port-design.md`; full acceptance checklist remains `2026-09-05-manga-port-handoff.md`.

## Global Constraints

- Work in `/Users/daniel/code/CrossInk` on `matcha_features`; never commit or push.
- Preserve dictionary implementation, font fixes, tests and existing handoff files.
- Exact Matcha v2 book compatibility; no second framebuffer; no implicit PSRAM reliance.
- No live OCR/cloud calls or credentials during tests.
- CTest runs sequentially because existing fixtures share paths.

## Task 1: Audit integration dependencies

- [x] Verify branch, worktree changes, pinned source, SDK and source license.
- [x] Audit binary writer and reader constants, bounds and metadata behavior.
- [x] Record reader/navigation/cache/prefetch/dictionary integration map in `2026-09-05-manga-reader-audit.md`.

## Task 2: Binary decoder foundation

Files: `lib/MangaPanel/MangaFormat.{h,cpp}`, `lib/MangaPanel/LICENSE`,
`test/manga_format/{CMakeLists.txt,MangaFormatTest.cpp,fixtures/*}`, `test/CMakeLists.txt`,
`docs/manga-format.md`.

Consumes pinned converter output; produces allocation-free index/page/panel/text/meta/TOC
views. Caller owns backing bytes. Header documents exact signatures and lifetime.

- [x] Write failing behavior tests for v2 records, Japanese OCR and translations, TOC,
  metadata with/without language, empty pages and truncated/overflowing records.
- [x] Generate tiny checked-in fixtures with pinned original writer, recording source SHA
  and inputs so tests are independent of the ported writer.
- [x] Implement checked little-endian reads, page validation and bounded iteration with
  no allocations; unsupported panel versions rejected; failures clear outputs.
- [x] Configure native CMake, build and run `ctest --test-dir build/task16-tests -j 1
  --output-on-failure -R MangaFormat` (or a separate build using cached GoogleTest).
- [x] Document exact byte contract and policy differences for malformed optional files.

## Task 3: Host converter

Files: `tools/manga_convert/convert_manga.py`, `tools/manga_convert/{README.md,LICENSE}`,
`test/manga_converter/test_converter.py`.

Consumes folders/CBZ/ZIP/EPUB/PDF; produces the Task 2 byte contract and canonical images.

- [x] Write offline tests before copying, exercising literal binary output, natural/explicit
  order, metadata/TOC extraction, sizing, mono output and coordinate mapping where practical.
- [x] Port pinned script retaining license, correct stale version/setup prose only unless
  a tested defect requires a scoped fix. Document optional dependencies and model download.
- [x] Audit actual recovery behavior; do not describe per-page rewriting as atomic/resumable.
- [x] Run Python unittest with network/model access blocked; record optional-dependency gaps.

## Subsequent integration tasks (not part of foundation completion)

- [x] Task 4: HAL MangaBook adapter with file failure/close tests and both image layouts.
  Complete under `2026-09-05-manga-storage-plan.md`: 21 adapter tests, full native
  suite 538/538, ASan/UBSan 21/21, actual C3 and Sticky/S3 HAL-header object compiles,
  independent review with no unresolved blockers. No firmware UI integration yet.
- [x] Task 5: Reader state-machine tests then page/panel activity, rotation, progress,
  per-book settings, bookmark and chapter menus, translated UI/touch integration.
- [x] Task 6: Browser/library/Home/Recent/Continue paths, cover thumbnails and safe actions.
  See `2026-09-06-manga-library-plan.md` and `2026-09-06-manga-library-review.md`;
  full parity still excludes language-attributed stats and external folder-state migration.
- [x] Task 7: Shared image geometry, disposable versioned caches and cached/fresh parity (device image/ghosting acceptance remains in Task 10).
- [x] Task 8: Cancellable bounded prefetch worker with storage coordination and lifecycle tests. Native 637/637 and deterministic simulator stress pass; physical/S3 gates remain in validation ledger.
- [x] Task 9: Panel OCR selection and verified persistent scans through unified dictionary interfaces (physical acceptance remains in the final gate).
- [ ] Task 10e: Complete the recovered pinned manga menu, page/panel status overlays,
  auto-turn navigation and unified-lookup shortcut routes before the final Task 10d gate.
- [ ] Task 10: Build default/sticky/x4-pro/simulators; record flash headroom and hardware
  image/ghosting/heap/rotation/navigation/resume matrix; restore dictionary backup safely.

## Progress

Initial state: tracked tree clean, dictionary preserved at `ea039400`; untracked
`.codegraph/` and `docs/superpowers/` retained. Work in place follows explicit user
instruction rather than creating a different branch/worktree. Foundation work does
not constitute on-device reader support or full manga parity.

Baseline validation: `ctest --test-dir build/task16-tests -j 1 --output-on-failure`
passed **504/504** on 2026-09-05 before implementation. The X4 serial port is occupied
by an existing Python monitor; this session does not interrupt it. Dictionary backup
restoration and hardware checks remain pending.

Task boundaries reviewed: Task 2 owns only binary decoding/native tests; Task 3 owns
the host converter/Python tests. Both consume the pinned binary contract and do not
change dictionary code. Task 2 fixtures use the original writer, independently of
Task 3's scoped OCR-coordinate correction. Shared CMake changes belong to Task 2.

Foundation validation: rebuilt `build/task16-tests`, **517/517 passed** sequentially;
converter `/usr/bin/python3 -m unittest discover -s test/manga_converter -v`, **9
passed, 1 skipped** (PyMuPDF unavailable). Decoder compiled with both actual
PlatformIO C3 and S3 compilers using `-std=gnu++2a -Os -fno-exceptions -fno-rtti
-Wall -Wextra -Werror`. This is object compilation, not a full firmware/link/OTA
measurement. Dictionary source and SDK remain unchanged.

Independent foundation review is complete in `2026-09-05-manga-foundation-review.md`:
no unresolved blocking findings. The zero-length page offset compatibility finding
was fixed with a failing-then-passing regression. A separate ASan/UBSan run passed
all 13 decoder tests with exceptions/RTTI disabled. Tasks 1–3 are complete for this
foundation slice.

Task 4 continuation is complete: `MangaBook` owns bounded reusable buffers, closes
all files before return, supports canonical and legacy page/crop layouts, and
provides optional metadata/TOC fallbacks. Review and validation are recorded in
`2026-09-05-manga-storage-review.md` and `2026-09-05-manga-port-validation.md`.
Resume at Task 5 (reader navigation state and activity); retain the adapter's
borrowed-buffer synchronization requirement when the render task reads page views.

Task 5 completed on 2026-09-06 under `2026-09-05-manga-reader-plan.md`.
Native 559/559, default/C3 and Sticky/S3 full firmware builds, both simulator builds,
manga button/touch smoke including Books-folder reopening and persisted state, and
existing EPUB/dictionary smoke pass. Review: `2026-09-05-manga-reader-review.md`.
Current continuation point: Task 6 (library/Home/recents/thumbnails/safe actions).
C3 OTA headroom is 24,272 bytes; keep measuring as the remaining port grows.
No hardware upload or Git publication occurred.

Task 6 library integration completed on 2026-09-06. Native **575/575**, both simulator
profiles, manga Home/Recents/cache/stat/sleep flows, additional Carousel/Minimal/
Dashboard theme runs and EPUB/dictionary smoke pass. Final default/C3 and Sticky/S3
builds pass at 6,542,528 and 6,329,200 bytes respectively. C3 OTA headroom is now
11,072 bytes. No Task 6 flash or Git publication; dictionary and SDK remain preserved.

Task 7 completed on 2026-09-06: shared geometry, validated MPX1 pixel caches,
cached source dimensions, and grayscale replay without a second framebuffer.
Native **599/599**, button/touch cold-warm BMP pixel comparisons, monochrome manga,
and EPUB/dictionary smoke pass. Final default/C3 and Sticky/S3 images are
6,551,008 and 6,336,464 bytes. C3 OTA headroom is **2,592 bytes**; remaining
features need flash-budget work while preserving dictionaries and user fonts.
Review: `2026-09-06-manga-pixel-review.md`. No Task 7 flash or Git publication.

Current continuation point: Task 10a cover cancellation and allocation safety,
then Tasks 10b/10c/10e. Task 9 OCR and verified scans passed independent review,
675 native tests, button/touch/EPUB simulator flows, and all hardware builds.
Task 8 software passed
independent review and cancellation stress; its C3 checkpoint remains unflashed.
Task 7 was subsequently flashed and the user reported "all good"; measured
physical JPEG/PNG, gray shades, ghosting and heap checks remain in the final gate.
Simulator parity covers real BMP decoding and replay.
Task 6 hardware follow-up and remaining full-port acceptance gaps are explicit in
the validation ledger. Folder move/rename is not exposed on-device; external moves
do not migrate manga state. Existing stats have no per-language dimension.

Menu/status parity ruling, 2026-09-06: the pinned inventory was recovered after
the initial reader slice. The current eleven-row manga popup covers the original
eight actions plus Task 9b lookup/translation/history, but it does not complete
filtered settings, exact-rate auto turn, screenshot, durable cache deletion or
offline OCR QR. It also omits pinned page/panel counters, the Panels hint and the
configured short-power/Quick Actions lookup routes. These are tracked as Task 10e
rather than waived as global equivalents. Task 10e depends on Task 9b/9c and must
preserve Task 10c's `dictionary.bin` and history contract during cache deletion.
The root handoff additionally assigns panel-mode Confirm directly to current-panel
lookup; Task 9b owns that routing and prefetch-drain coverage. Task 10e preserves
it and reaches the panel menu through touch/global menu entry instead.
