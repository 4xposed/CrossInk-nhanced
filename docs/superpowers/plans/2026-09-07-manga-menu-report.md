# Manga menu completion report — Task 10e

Date: 2026-09-07. Worktree: `matcha_features`. No commits, staging, pushes, SDK changes, model downloads, or hardware/serial access by this implementer. Root owns all PlatformIO and simulator gates. This report covers the Task 10e delta; existing dictionary and Tasks 1–10c work remain outside it.

## Result

Implementation, independent review and task native/simulator acceptance are complete after three scoped correction rounds. Final whole-port all-board rebuilds and physical acceptance remain separate Task 10d gates.

The existing paged OptionPopup exposes all sixteen pinned commands from page overview and panel view. Manga uses the shared settings widgets with only applicable input controls. Automatic turning has session-only Off/1/3/6/12-per-minute state, a single wrap-safe deadline, overview page skipping, normal panel transitions, cancellation, and busy-work deferral. Both configured lookup overloads use the same current-view OCR flow.

Status labels use oriented safe bounds. BW renders opaque white patches and black glyphs; both grayscale masks clear the identical rectangles to zero, and cleanup retains the restored labeled BW frame. Screenshots run after popup dismissal and successful restoration, use manga title/page/progress metadata, and show translated failure feedback. Confirmed cache deletion drains work, saves progress/statistics/bookmarks, closes source/cache files, removes only derived data, and returns Home after success feedback. OCR QR assembly owns bounded original-text bytes in stored order, excludes translations, checks strict UTF-8, and moves ownership into the existing QR child.

## Files and baseline

Initial baseline: `/private/tmp/crossink-manga-menu10e-before`, captured on live-source release before integration. Additional BookmarkStore and native-stub preimages were captured before their correction. Root recovered exact earlier preimages for OptionPopup, SimulatorSmokeTest, and the Python smoke script from prior task snapshots/patches; provenance and SHA-256 values are in `/private/tmp/crossink-manga-menu10e-baseline-recovery/provenance.md`. No removed-addition guessing was used.

Modified existing files:

- `CHANGELOG.md`
- `lib/I18n/translations/english.yaml`
- `scripts/run_manga_simulator_smoke_test.py`
- `src/BookmarkStore.cpp`
- `src/BookmarkStore.h`
- `src/SettingsList.h`
- `src/activities/reader/MangaNavigation.cpp`
- `src/activities/reader/MangaNavigation.h`
- `src/activities/reader/MangaReaderActivity.cpp`
- `src/activities/reader/MangaReaderActivity.h`
- `src/activities/reader/QrDisplayActivity.cpp`
- `src/activities/reader/QrDisplayActivity.h`
- `src/activities/reader/ReaderOptionsActivity.cpp`
- `src/activities/reader/ReaderOptionsActivity.h`
- `src/components/OptionPopup.h`
- `src/simulator/SimulatorSmokeTest.cpp`
- `src/util/QrUtils.cpp`
- `src/util/QrUtils.h`
- `src/util/ScreenshotInfo.h`
- `src/util/ScreenshotUtil.cpp`
- `src/util/ScreenshotUtil.h`
- `test/CMakeLists.txt`
- `test/reading_language_stats/stubs/HalStorage.cpp`
- `test/reading_language_stats/stubs/HalStorage.h`
- `test/reading_language_stats/stubs/Logging.h`

New files:

- `src/activities/reader/MangaMenuState.h`
- `src/activities/reader/MangaQrPayload.h`
- `src/activities/reader/MangaQrPayload.cpp`
- `src/activities/reader/MangaStatsCommit.h`
- `src/activities/reader/MangaStatsCommit.cpp`
- `src/activities/reader/MangaStatus.h`
- `src/activities/reader/MangaStatus.cpp`
- `src/util/QrCodePolicy.h`
- `src/simulator/MangaStatusSmoke.h`
- `src/simulator/MangaStatusSmoke.cpp`
- `docs/superpowers/plans/2026-09-07-manga-menu-report.md`
- `test/manga_menu/CMakeLists.txt`
- `test/manga_menu/MangaBookmarkSaveTest.cpp`
- `test/manga_menu/MangaMenuTest.cpp`
- `test/manga_menu/MangaQrTest.cpp`
- `test/manga_menu/MangaScreenshotWriteTest.cpp`
- `test/manga_menu/MangaStatsCommitTest.cpp`
- `test/manga_menu/MangaStatusPixelsTest.cpp`
- `test/manga_menu/MangaStatusTest.cpp`
- `test/manga_menu/QrDrawTest.cpp`
- `test/manga_menu/QrPolicyTest.cpp`
- `test/manga_menu/screenshot_stubs/Arduino.h`
- `test/manga_menu/screenshot_stubs/FsHelpers.h`
- `test/manga_menu/screenshot_stubs/GfxRenderer.h`
- `test/manga_menu/screenshot_stubs/activities/Activity.h`
- `test/manga_menu/stubs/GfxRenderer.h`
- `test/manga_menu/stubs/components/themes/BaseTheme.h`

I18n outputs were regenerated using `python3 scripts/gen_i18n.py` from English YAML, never edited manually. Generated outputs and Python bytecode are excluded from the review delta. No storage format changed in this task. The pinned SDK remains `1e8ee543edca397f2b8747811f5a88f1bc35d233`.

## Durable retry behavior

`MangaStatsCommit` stores one fixed frozen book/global/span transaction and per-target acknowledgments. An already successful target is never replayed. A failed target receives its original snapshot/span before newer page/completion changes or accepted active time are banked. The activity retains the unbanked tail, including fractional milliseconds. Each paused attempt can finish the old transaction and publish at most one new tail transaction. Session and start-date thresholds are cumulative for the activation: 20+5 seconds retains 25, 40+25 counts one session, and 120 seconds establishes the automatic start date once. The language and compressed local-start convention match the approved existing statistics convention.

Once the cache precommit path is used, managed suspension also checks pending progress, stats, and bookmarks. Persistence failure cancels the transition, restores reader input and visible retry feedback, and suppresses automatic retry loops. Prefetch Draining still waits. This is activity-local retry state, not a durable transaction queue.

`BookmarkStore::saveToFileChecked()` retains dirty state on failed writes, sync, close, and empty-store removal. The existing void method remains a compatibility wrapper. The writer uses existing checked serialization primitives; no bookmark format, migration ownership, or store architecture changed.

## Regressions investigated before fixes

- Closed-book cache feedback: `MangaReaderActivity::deleteCacheWhenReady()` closed the book but left `ready` true; the next popup background render attempted to decode it. RED actual simulator log `/private/tmp/crossink-menu10e-cache-red.log` contains the page-render error and exits 2. The fix clears readiness and pixel state and gives successful deletion feedback a blank background. The internal smoke assertion now checks the reader render-error counter before declaring success. GREEN `/private/tmp/crossink-menu10e-cache-green.log` exits 0.
- Screenshot after failed restored BW: grayscale restoration could fail while render still called ScreenshotUtil on the error screen. The renderer now returns restoration success and screenshot capture requires a valid frame; otherwise it queues the translated page-load error. The SIMULATOR-only, default-inactive injection forces both restoration and fallback failure in the actual gray pipeline. RED `/private/tmp/crossink-menu10e-restore-red.log` exits 2 with the missing-feedback assertion; GREEN `/private/tmp/crossink-menu10e-restore-green.log` exits 0 and checks that no BMP was written. Preimages: `/private/tmp/crossink-manga-menu10e-before-renderfix`.
- Bookmark failed write/removal: native tests against actual BookmarkStore and real host files failed before correction: retry after a short write could not reload the bookmark, and retry after failed empty removal left the old file. RED logs `/private/tmp/crossink-menu10e-bookmark-{write,remove}-red.log` record assertion failures. GREEN write/sync/close/remove logs at `/private/tmp/crossink-menu10e-bookmark-*-green.log` all exit 0 and verify successful retry and zero open handles.
- The first extended cache retry fixture used marker byte 73 in `stats_v4.bin`; the existing future-version guard correctly refused mutation. The fixture now creates the documented 69-byte v4 layout (version 4, zero counters/flags/unknown dates, per BookReadingStats.cpp), and verifies those bytes survive. Production future-version refusal is unchanged.

- Screenshot close status: the existing BMP writer ignored the final close result and the newly checked caller could therefore report success after a late SD failure. Native test `MangaScreenshotWriteTest` uses the actual ScreenshotUtil writer, actual BitmapHelpers/header code, and injectable host storage; UI/renderer dependencies are stubbed and are not exercised by this test. RED `/private/tmp/crossink-menu10e-screenshot-close-red.log` records the false-success assertion; GREEN `/private/tmp/crossink-menu10e-screenshot-close-green.log` verifies close failure, write failure, cleanup, eventual retry, and zero open handles. Preimage `/private/tmp/crossink-manga-menu10e-before-screenshot-close/src/util/ScreenshotUtil.cpp`.

The first C3 compile also exposed incorrect uses of the literal-only `tr` macro. Dynamic IDs now use qualified `StrId` and the existing `I18n::getInstance().get` API. The corrected build passed; the macro itself was not changed.

## Validation evidence

Completed before the final extended gate:

- Seven native CMake tests passed, 7/7 in 2.23 seconds: stable menu mapping, all auto rates/deadline wrap/defer/cancellation/end transitions, overview skipping/panel navigation, safe layout, exact BW/mask/restored patch bits, original OCR ordering/empty/malformed/multibyte cap/OOM/ownership, real pinned QR codec capacities and checked drawing, and per-target statistics retry including resumed time/page/completion changes and cumulative thresholds. Logs `/private/tmp/crossink-menu10e-native-{config,build,tests}.log`.
- All sixteen menu rows from overview and panel passed on both button and touch simulators, in monochrome and grayscale. Logs `/private/tmp/crossink-menu10e-menu-{simulator,sticky-simulator}.log` and `/private/tmp/crossink-menu10e-menu-gray-{simulator,sticky-simulator}.log`. The real GfxRenderer fixture seeds nonzero gray masks, checks pixels in all four orientations with asymmetric insets, executes `displayGrayBuffer()` and restored-BW cleanup, and checks the baseline hash. This is separate from the small native raster test adapter.
- Existing button/touch OCR flows passed, including shared lookup, persistence and canceled-statistics-sleep behavior: `/private/tmp/crossink-menu10e-initial-{button,touch}-smoke.log`.
- Existing EPUB reader/Reader Options smoke passed: `/private/tmp/crossink-menu10e-epub-smoke.log`.
- C3 compilefix1 passed in 60.900 seconds: image 6,499,120 bytes, 54,480 bytes remaining, SHA-256 `83639b5576fd8bcfe84598b4f601410dd47668fddb6bf178c6a24a36eca308ae`. This is a checkpoint measurement, not the final bookmark correction image or an optimization claim.

Final extended gate evidence:

- Eleven CMake tests passed, 11/11 in 0.97 seconds including bookmark write/sync/close/remove: `/private/tmp/crossink-menu10e-bookmark-native-tests.log`.
- Six enhanced simulator variants passed: overview/panel menu on both profiles in mono and gray (61.8/63.4 and 62.6/63.9 seconds), plus actual failure/retry flows on both profiles (19.6 seconds each). Logs `/private/tmp/crossink-menu10e-bookmark-{menu,menu-gray,failures}-{simulator,sticky-simulator}.log`. These verify an actual 12/min advance and ignored manual turn, settings widget change/reload, both shortcut enum overloads under modal/lock/suspend guards, exact stored BMP pixels versus labeled reader framebuffer plus filename/page/title/progress, global-stat failure followed by resumed reading/new panel state and failed Home exit, eventual accepted-tail commit, dictionary/history/legacy-stat preservation, cache regeneration, idempotent exit, and screenshot SD failure feedback.
- C3 + simulator pair build passed in 85.750 seconds: C3 66.858 seconds; button simulator 9.604; touch simulator 9.289. C3 image 6,499,680 bytes, 53,920 bytes remaining, SHA-256 `a128a8690bb86affec72d24480cc7912583249ec043cf755995a2d24a8792129`. Log `/private/tmp/crossink-menu10e-bookmark-builds.log`. This precedes only the final screenshot-close/log correction.
- Final screenshot-close CMake case, final target rebuilds, S3 builds and root review are pending at this draft. No physical-device acceptance is claimed.

Reproduction (root owns PlatformIO and simulator processes):

```sh
cmake -S test -B build/task16-tests
cmake --build build/task16-tests --target MangaMenuTest MangaStatusTest MangaStatusPixelsTest MangaQrTest MangaStatsCommitTest QrPolicyTest QrDrawTest MangaBookmarkSaveTest MangaScreenshotWriteTest
ctest --test-dir build/task16-tests --output-on-failure -R 'Manga(Menu|Status|Qr|StatsCommit|BookmarkSave|ScreenshotWrite)|Qr(Policy|Draw)'
CROSSINK_SIMULATOR_MANGA_OCR=1 CROSSINK_SIMULATOR_MANGA_MENU=1 python3 scripts/run_manga_simulator_smoke_test.py --env simulator --no-build --timeout 180
CROSSINK_SIMULATOR_MANGA_OCR=1 CROSSINK_SIMULATOR_MANGA_MENU=1 CROSSINK_SIMULATOR_MANGA_GRAYSCALE=1 python3 scripts/run_manga_simulator_smoke_test.py --env sticky-simulator --no-build --timeout 180
CROSSINK_SIMULATOR_MANGA_OCR=1 CROSSINK_SIMULATOR_MANGA_FAILURES=1 CROSSINK_SIMULATOR_MANGA_GRAYSCALE=1 python3 scripts/run_manga_simulator_smoke_test.py --env simulator --no-build --timeout 120
```

Run the menu and failure variants on both simulator profiles; mono and gray menu modes exercise the same menu mapping. Failure fixtures use isolated temporary filesystems only. Expected storage errors are filtered by their specific messages; unexpected MANGA/BKS errors and crash patterns still fail.

## Memory and limits

- `sizeof(manga::StatsCommit)` is 404 bytes in native measurement; a compile-time ceiling of 512 bytes applies on every target. It is an inline activity member, avoiding both a fallible transaction allocation and large stack snapshots. AutoTurn uses one deadline and flags, not a timer queue.
- OCR payload allocation is at most 2,954 bytes including NUL; QR module grid at most 3,917 bytes. Both use nothrow owned arrays. Payload allocates on explicit request; the child allocates the grid once on entry and frees resources on exit. Existing string callers retain compatible bounded wrappers without adding manga text copies. No second framebuffer is allocated.
- Byte capacities for versions 4/10/20/30/40 at ECC_LOW are 78/271/858/1732/2953, checked before `qrcode_initBytes`. Lowercase/binary/multibyte tests ensure byte-mode bounds rather than optimistic numeric capacities. The pinned codec return value does not protect against oversized writes.
- QR encoding remains on the existing 16,384-byte render task. Pinned source `.pio/libdeps/default/QRCode/src/qrcode.c` has simultaneous v40 raw arrays 3,706 codewords + 3,917 function grid + nested 3,706 error-correction result + 30 coefficients = 11,359 bytes. Alignment-pattern scratch is a separate, earlier branch. Root isolated GCC 14.2 `-Os` C3 assembly rounds these VLAs to 3,712 + 3,920 + 3,712 + 32 plus the 176-byte fixed frame = 11,552 bytes before other caller/library frames. Therefore at most 4,832 bytes remain for those frames and physical margin. `.su` saying `176 dynamic` excludes the VLAs. This is an isolated compiler probe, not exact full-build stack usage or physical safety proof.
- Live QR chain: ActivityManager render task → QrDisplayActivity::render → QrUtils::drawQrCode → qrcode_initBytes → inlined performErrorCorrection → Reed–Solomon helpers; earlier function-pattern drawing is a separate peak. Existing grid/payload are heap-owned, not extra caller VLAs. Probe artifacts `/private/tmp/crossink-qr-{c3,s3}.{su,s}`. No stack increase, vendor codec replacement or high-water heuristic was introduced.
- Status uses fixed small counter text and layout rectangles; it replays existing caches through the existing scratch area. Screenshot feedback inverts and restores border pixels. Native/simulator success does not measure C3 heap fragmentation or physical display ghosting.

Physical acceptance still required on C3 X4/X3 and S3 Sticky/X4 Pro: exercise all rates, rapid Confirm/Back/touch/lock/suspend cancellation while JPEG/PNG/BMP and prefetch are active; maximum mixed-UTF-8 QR and SD failure paths; screenshot success/failure and same-view return; all rotated/inset labels and grayscale ghosting; free/largest internal heap, S3 PSRAM, and render/prefetch stack high-water marks. Use derived-only cache clearing for cold/warm comparisons and retain reading state. Physical hardware was not accessed for this task.


## Review correction round 1

Review `2026-09-07-manga-menu-review.md` identified two actual state/dispatch defects. Exact preimage snapshot: `/private/tmp/crossink-manga-menu10e-review-fix1-before`.

- R1: pending rendering was serviced after the auto branch's unconditional return. A worker-drained menu could also fulfill a render without clearing the old flag. Deferred requests are now serviced before the auto branch, and every fulfilled render retires the pending flag. The source-open and held-completion simulator sequence refresh→deferred render→menu→rate selection fails before the fix and advances after one rearmed deadline afterward.
- R2: `idle()==false` conflated ordinary warming with an already-owned foreground cancellation. One activity-local boolean now tracks foreground drain ownership, clearing when the worker becomes idle. A one-shot shortcut during ordinary Running/Finished warming retains the existing lookup intent and requests cancellation; an already-owned drain rejects a competing shortcut. The menu smoke no longer retries a rejected lookup or forces refresh to make it pass.
- Deterministic RED logs: `/private/tmp/crossink-menu10e-reviewfix1-{deferred,running,finished}-red.log`, all exit 2 with the respective stranded-auto/discarded-one-shot assertions. GREEN versions passed on button and touch: `/private/tmp/crossink-menu10e-reviewfix1-{deferred,running,finished}-{simulator,sticky-simulator}-green.log` (deferred approximately 7.5 seconds, shortcut cases 1.5 seconds).
- R3 QR: the real pinned codec now receives mixed 1/2/3/4-byte UTF-8 at every byte-capacity threshold, with guard bytes after its module grid and deterministic repeated encodes. An actual activity flow renders a maximum 2,953-byte mixed payload twice, injects failure at the real child-allocation boundary and at its reusable-grid allocation, checks translated feedback, and returns to the same view. QR simulator probes passed on both profiles: `/private/tmp/crossink-menu10e-reviewfix1-qr-{simulator,sticky-simulator}-green.log` (approximately 5 seconds). Injection flags are SIMULATOR-only and inactive by default.
- R3 statistics: native tests add book-failure/global-success, both targets failing and recovering independently, and T1 completion followed by a partial T2 failure. They inspect real persisted daily/language rows across midnight, preserve failed snapshots against later live page changes, assert exact row totals and session count, and verify 900 ms retained from a previous accepted total plus 200 ms becomes one later saved second. Expanded native QR/stats tests pass 2/2: `/private/tmp/crossink-menu10e-reviewfix1-native-tests.log`.
- R3 final UI assertions now cover active Confirm/Back/lock/child suspension/exit and touch-menu cancellation, touch-only navigation of all menu rows, and forced landscape popup paging in both directions followed by reopen and fresh first/last-row selection. Their final simulator gate is pending at this update.
- R4 warnings at main.cpp's lock badge narrowing and SettingsActivity's unrelated switch cases are outside this delta and remain assigned to root's final Task 10d triage. No warning-free claim is made. Cross-task 9b/9c/10c acceptance remains with their accepted reports; physical QR stack margin, display ghosting and device acceptance remain unverified.

The additional production state is one boolean within the existing activity; no timer, worker, framebuffer or additional heap buffer was added by R1/R2. QR allocation fault injection and all new event/paging machinery compile only for the simulator.


## Review correction round 2

The scoped re-review accepted original R1/R2 and the QR/statistics R3 matrices, then identified an input-order regression from R1's early service placement (R5) and a smoke-harness validation bypass (R6). Exact five-file preimages were captured before any round-2 edits at `/private/tmp/crossink-manga-menu10e-review-fix2-before`; the only new path is `test/manga_menu/MangaSmokeValidationTest.py`.

- R5 production change is confined to `MangaReaderActivity.cpp:879` and `:988`. Active Confirm/Back/touch cancellation runs before active deferred-render service. Ordinary deferred rendering runs after manual input capture and retained-intent consumption. Fulfilled rendering still retires the flag at `:1292`. No member, allocation, worker or stack buffer was added.
- The simulator regression (`SimulatorSmokeTest.cpp:759`) uses the existing held-source and held-completion worker barriers. After a real manual refresh defers, it injects exactly one PageForward release in the iteration which makes the completed worker result consumable. A second mode starts actual automatic turning, then injects Confirm at that same completion boundary. Both check again after additional frames to exclude replay. Neither uses a timing sleep to align completion.
- Actual RED evidence: `/private/tmp/crossink-menu10e-reviewfix2-release-gray-red.log` exits 2 with “Deferred render discarded or replayed one-shot page release”; `/private/tmp/crossink-menu10e-reviewfix2-active_cancel-gray-red.log` exits 2 with “Deferred render consumed active cancellation edge”. The earlier non-gray runs only failed setup and are not RED evidence: a 1-bit BMP is skipped before the worker fingerprint hook (`MangaPrefetch.cpp:192`); these tests require `CROSSINK_SIMULATOR_MANGA_GRAYSCALE=1`.
- R6 moves only the ACTIVE_EVENTS and TOUCH_PAGING completion branches below the shared crash/reader/storage error scan and common simulator-success marker (`run_manga_simulator_smoke_test.py:323`). Existing expected-error filters are unchanged. The new native Python case calls the real `run_smoke`, replacing fixture setup and the subprocess result. For both modes it checks a clean success, injected recoverable `[ERR] [MANGA]` despite zero exit/all markers, and a missing common final marker. RED had four failing subtests; GREEN passes all three tests, six subcases. Logs `/private/tmp/crossink-menu10e-reviewfix2-harness-{red,green}.log`. It is registered as `MangaSmokeValidationTest` in the native CMake suite.
- R5 GREEN simulator execution and the previously added active-event/touch-paging/full-touch-menu gates remain pending at this source freeze. Root's preceding round-1 C3/simulator pair build passed in 159.313 seconds (143.175/8.248/7.891); initial S3 builds passed Sticky/X4Pro in 221.033/120.828 seconds. These earlier builds are checkpoints, not validation of the new ordering correction.

Root reproduction for each profile (`simulator`, `sticky-simulator`), with the current binary rebuilt first:

```sh
CROSSINK_SIMULATOR_MANGA_OCR=1 CROSSINK_SIMULATOR_MANGA_GRAYSCALE=1 CROSSINK_SIMULATOR_MANGA_REVIEW=release python3 scripts/run_manga_simulator_smoke_test.py --env simulator --no-build --timeout 90
CROSSINK_SIMULATOR_MANGA_OCR=1 CROSSINK_SIMULATOR_MANGA_GRAYSCALE=1 CROSSINK_SIMULATOR_MANGA_REVIEW=active_cancel python3 scripts/run_manga_simulator_smoke_test.py --env simulator --no-build --timeout 90
python3 -B test/manga_menu/MangaSmokeValidationTest.py
```

Physical follow-up remains pending: on C3 and S3, refresh while an image is warming and release a page button as warming drains; exactly one view change should occur without a second press. In active auto mode, Confirm/Back/touch at that boundary must cancel without opening a menu or changing view. All earlier QR physical-stack/heap and display acceptance limits remain unchanged.


## Touch-only execution correction

The pending R3 touch-only gates exposed a simulator input classification defect; they were not GREEN. Failure logs are `/private/tmp/crossink-menu10e-reviewfix2-touch-paging-green.log` ("Popup swipe selected a stale touched row") and `/private/tmp/crossink-menu10e-reviewfix2-touch-menu[-gray]-green.log` (unexpected LookedUpWords child followed by wrong-activity assertion). Their filenames describe the attempted gate, not its result.

`MappedInputManager.cpp:282` treated every injected release as a tap, while its synthetic `decodeSwipe` also classified a moved release as a swipe. `OptionPopup.h:150` handles taps before swipes (`:183`), so the synthetic gesture selected its touch-down row. The pinned SDK's real `InputManager::wasTouchTap` (`freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp:585–593`) already excludes releases whose movement reached swipe distance. The correction is confined to the SIMULATOR branch: reuse the existing synthetic swipe decision to suppress its simultaneous tap. No physical input threshold, popup behavior, SDK file, or hardware code path changes. Existing suppression and long-press checks retain their order.

Exact preimages: `/private/tmp/crossink-manga-menu10e-touch-injection-before` (MappedInputManager.cpp, SimulatorSmokeTest.cpp, this report). The same actual touch paging/selection scripts now additionally assert moved release → swipe and no tap, and stationary release → tap at its down position and no swipe. The prior three failing behavioral executions establish RED; corrected executions remain pending at this freeze. This corrects simulator fidelity rather than replacing swipe tests with button navigation.

## Root task acceptance

Independent review approved all scoped corrections through round3. Current
C3 + simulator pair PASS52.434s after input-order correction; C3 image6,499,840
bytes/free53,760 SHA453a051918b1285f302c96e2420c6c79942d4fc38e489058dd97e8fd1b149422.
The later correction is SIM-only swipe/tap exclusivity. Fresh sim pair14.260s
explicitly compiled MappedInputManager and SimulatorSmokeTest in both profiles.
Touch-only landscape paging passed2.45s; full16row touchmenu mono/gray passed
53.6/53.8s. Logs /private/tmp/crossink-menu10e-touch-injection-
{touch-paging,touch-menu,touch-menu-gray}-green.log. Same-frame release/cancellation,
deferred auto, and active Confirm/Back/lock/child/exit/touch cases passed both
profiles in /private/tmp/crossink-menu10e-reviewfix2-
{release,active_cancel,deferred,events}-{simulator,sticky-simulator}-green.log.
Harness CMake test also passed. The extra boundaryRED build had a source-freeze
message race and is not stable-preimage evidence; the three earlier touch failures
remain behavioralRED, and all GREEN binaries used freshly frozen source.

Task10e software accepted. Final10d review/fixes will precede fresh all-board
artifacts; older S3 checkpoint passes do not prove current-source S3 acceptance.
No hardware flash or physical acceptance performed.
