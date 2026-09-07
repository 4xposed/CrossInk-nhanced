# Combined final manga correction wave — 2026-09-07

## Status and scope

F1–F6 and M1–M3 have been addressed together. **Production source, tests and public
documentation are functionally frozen for root's fresh gates and one scoped
re-review.** This report is the only remaining documentation bookkeeping. No PIO,
simulator process, serial session, device write, user SD access, export, cloud/model
call, staging, commit, push or SDK/ref mutation was performed by this fixer.

The original final review remains the authority for whole-port coverage: all 363
manifest entries, all 50 handoff checkboxes, binary provenance and the complete
51-diagnostic static triage. This correction is not another broad port review.
Dictionary implementation and assets were preserved. All 307 original preimage
hashes still match `/private/tmp/crossink-manga-final-fix-before/manifest.txt`.
There are 34 changed existing paths, all already in that snapshot; no additional
existing-file preimage was needed. New files are separately inventoried below and
in `/private/tmp/crossink-manga-final-fix-new-files.txt`.

Focused verification: **131/131 CTest entries passed**, including the registered
source-boundary test (six cases). Ordinary converter discovery independently
passed **10/10**, with installed PDF support and no fitz preimport. These are scoped
host results, not final firmware/simulator or physical acceptance. Fresh all-board,
full native, real WebSocket lifecycle and the prepared complete simulator matrix
remain root-owned gates.

## F1 — transfer quiescence no longer waits for a suppressed loop

- `src/activities/network/CalibreConnectActivity.h:47` and
  `CrossPointWebServerActivity.h:89` invoke main-owner cancellation from
  `prepareToSuspend()` before reporting readiness.
- `src/network/CrossPointWebServer.cpp:418` implements `cancelActiveUploads()`;
  the declaration is `CrossPointWebServer.h:68`. It explicitly closes the current
  WS/HTTP/font file, drops HTTP/font buffered bytes, clears completion ownership,
  and applies the existing best-effort partial-file removal policy. HTTP closes
  its active client; late WS binary frames see no active transfer and cannot write.
  The WS path performs no socket send/flush during quiescence, avoiding another
  client-dependent wait. The server remains running for a pushed activity's return.
  `stop()` reuses the same cleanup.
- Navigation and deep-sleep retry both call this hook on the main owner under the
  existing RenderLock contract. No background callback/storage worker was added;
  pending input does not replace the queued intent. Existing direct-restart Web
  exits and normal completed transfers retain their policy.

RED/GREEN: `test/source_contracts/test_upload_suspension.py` compiles the exact two
production suspension hooks. With an incomplete transfer and no ordinary loop,
old hooks fail readiness; fixed hooks release the owner. Logs:
`/private/tmp/crossink-manga-final-fix-f1-red.log` and `...-f1-green.log`.
This uses a transfer-owner double and does **not** claim a real WebSocket pass.

The real localhost gate is prepared separately. The SIM-only existing smoke tick
recognizes `CROSSINK_SIMULATOR_UPLOAD_SUSPEND=home|pop|sleep` plus
`/upload-suspend.trigger` in an isolated simulator SD. The external client writes
the trigger only after actual START/READY and incomplete binary data. The tick
calls the real manager Home/pop or `enterDeepSleep(false)` path. Sleep temporarily
uses the already-existing sleep-return seam, restoring its environment afterward.
Root must require the post-trigger main marker
`Verified main sleep preparation completed after worker quiescence`, the later
`Upload sleep call returned`, and partial-file absence; a returned deferred call
alone is insufficient. Home/pop expect the normal completed transition/restart
behavior and absent partial. The HTTP listener-thread simulator behavior alone
is not evidence for hardware-owned WS progress.

Hardware: interrupt a throwaway upload on C3 and S3 via Calibre Back, managed Home
and sleep; verify bounded transition, explicitly closed file and the existing
partial-removal outcome. No hardware timing claim is made.

## F2 — sparse/mixed canonical pages keep physical identity

`lib/MangaPanel/MangaBook.cpp:332,357,483` detects canonical page zero directly and
uses per-physical-page `.jpg`, `.jpeg`, `.bmp`, `.png` priority. Holes return
Missing instead of compacting neighbouring images. A mixed middle extension is
resolved at its encoded page number. Page-zero canonical books remain directory
free, with at most four probes per requested page. Missing-cover books classify
canonical names once, reusing the existing fallible bounded scan owner; only
noncanonical families use the existing dense legacy ordering. No full filename
vector, full index allocation or per-turn canonical directory scan was added.

Tests in `test/manga_book/MangaBookTest.cpp` changed the obsolete single-extension
assertion and added independent sparse/mixed layouts: absent final overview,
cover-plus-crops, retained middle overview, `.jpeg`, duplicate priority and missing
cover. The sparse four-page index is literal test data; original-writer page-zero
OCR remains independently checked against its own physical page. Tests assert
exact paths and missing-overview crop fallback without rewriting portable files.

RED: all three initial interoperability cases failed in
`/private/tmp/crossink-manga-final-fix-f2-red.log`. Final **29/29 MangaBook tests**
pass in `...-f2-f5-green.log`, including the subsequent real cancellation cases.
Two temporary failures during implementation exposed an allocation/discovery
lifetime regression; classification was moved to the first image request, keeping
optional metadata failures and original fallible-open behavior intact.

Hardware: copy the same disposable mixed/sparse/panel-only book unchanged to C3
and S3, confirm physical page/OCR alignment and crop fallback through jumps/reopen.
Clear only disposable caches for cold comparisons.

## F3 — both Recent deletion callbacks use the shared journal

`RecentBooksActivity.cpp:202` and `RecentBooksGridActivity.cpp:527` route selected
manga roots through `BookFolderMutation::remove()`. They reload only on Complete;
RecoveryPending and other failures are logged by the shared result string and
shown using existing translated recovery/error text. The engine owns checked
bookmark/progress/cache/ordinary-nested-file cleanup, matching resume-path edits,
and final owner reload. Existing non-manga deletion behavior is retained.

`test/book_folder_mutation/RecentDeleteTest.cpp` runs the **verbatim production
list/grid action callbacks**, extracted by a dependency-tracked CMake command,
against the real mutation engine, journal, JSON and bookmark cleanup code. Only UI,
metadata recognition and non-manga legacy collaborators are doubled. All eight
cases failed on the former handlers; all eight pass after correction. Cases cover
success/resume-path cleanup, partial physical deletion with survivor metadata,
metadata-cache failure followed by recovery, and recreated-path preservation.
An empty resume path prevents resumption; the existing journal does not rewrite
unrelated boolean fields merely because the path was cleared.

Logs: `/private/tmp/crossink-manga-final-fix-f3-red.log`, `...-f3-green.log`.
Two test expectations were corrected during GREEN investigation: existing
`lastSleepFromReader` retention after path clearing, and using an actual metadata
cache failure rather than a pre-outcome power cut when expecting Complete recovery.
The original RED failures remain valid route failures, not claimed exact-final
assertion coverage for those adjusted expectations.

Hardware: delete disposable bookmarked/progressed manga from both Recent layouts,
then restart after injected partial/metadata failures; compare cleanup and retained
survivor/recreated paths with Books deletion.

## F4 — completion edits retain snapshots and remain explicitly retryable

- `BookActions.cpp:192` now takes an activity-owned `CompletionEdit`, declared in
  new `BookCompletionEdit.h`. It loads book/global snapshots once, freezes the
  desired completion/date, and keeps `ReadingStatsEditState`'s successful-target
  mask. Read failures leave the operation uninitialized, so retry may reload safe
  snapshots; they cannot publish an empty replacement.
- `BookActions.cpp:265` persists only missing targets. The side-effect flag ensures
  recents/move effects run once after both required files publish, even if the
  helper is accidentally called again after success.
- New `BookCompletionActivity.{h,cpp}` owns the bounded transaction and existing
  OptionPopup retry UI. The activity is fallibly allocated; its two summary objects
  and retained metadata live on the heap rather than the C3 task stack. Popup
  changes follow RenderLock. Failed edits block unsafe suspension, with
  `cancelSuspensionOnFailure()` restoring ordinary input; Retry on buttons/touch
  explicitly tries the same frozen operation. Back/dismiss does not lose dirty
  edits. Neither suspension nor exit performs hidden saves.
- All four library callers use `startCompletionEdit()`: the ordinary and manga
  Books action routes (`FileBrowserActivity.cpp:504,688`) and both Recent layouts
  (`RecentBooksActivity.cpp:329`, `RecentBooksGridActivity.cpp:653`). Success feedback
  and view reload wait for the modal's completed result.

RED compiled the original real completion function with real statistics stores:
repeated book failures changed global count 7 to **10**, expected 8.
`test/reading_language_stats/CompletionActionTest.cpp` now passes four parameterized
cases covering both partial-success directions, persistent failure, eventual retry,
manual date retention, automatic date across a changed test clock and one-time
side effects. Successful repeated helper calls perform zero further writes.
Logs: `/private/tmp/crossink-manga-final-fix-f4-red.log`, `...-f4-green.log`.
`test_completion_lifecycle.py` additionally compiles the exact modal hooks/methods:
repeated suspension/dismissal does not save or destroy the failed owner; explicit
retry remains usable and exit after success does not replay writes.

No cross-file durable journal or power-loss atomicity claim was introduced.
Hardware: inject a save failure for each target on a disposable book, retry later,
and verify status, finished date and device count change exactly once.

## F5 — expired full-cover work cannot restart source discovery

`SleepCoverAssets.cpp:71,101,160` propagates the same borrowed token through manga
open, source path discovery and dimension probing, with polls before/after codec
operations. `MangaBook.cpp:87,105,357,483` polls index records, directory entries and
direct probes. The token is never retained by MangaBook. Conversion already had
cooperative checkpoints; those remain intact, including source-path resolution
in `MangaCover.cpp:352`.

Full preparation optionally returns its validated/published cache path.
`SleepActivity.cpp:733` checks already-expired budget before new manga work and
uses that returned path directly. Cancellation falls back to the default/custom
screen, with no post-expiry index reopen or dimensions pass. Cover handles are
explicitly closed before returning. The separate cached-path utility accepts an
optional token for callers that do need discovery; the sleep manga path no longer
uses it for fallback.

Four exact SleepCoverAssets/SleepActivity integration cases were RED and are GREEN:
expired-before-start, expiry during discovery, success without a second source
pass, and conversion cancellation. Source/codec doubles count operations here;
real MangaBook tests separately verify that mid-index cancellation stops reads,
closes handles, and interrupted directory classification retries correctly.
Logs: `/private/tmp/crossink-manga-final-fix-f5-red.log`, `...-f5-green.log`,
`...-f2-f5-green.log`, `...-boundaries-green.log`.

A single SD or codec call can overshoot; 2500 ms remains cooperative policy, not a
measured guarantee. Physical C3/S3 verification must record logged stage, elapsed
and maximum poll gap on slow/large disposable books.

## F6 — ordinary PDF discovery keeps native modules alive

`test/manga_converter/test_converter.py:45` now owns and restores only the
`huggingface_hub` and `ultralytics` module-table keys. The entire mapping is no
longer reset. Socket/subprocess/environment/model guards remain. No fitz preimport,
installed-PDF skip, dependency unload workaround or live service call was added.

Fresh ordinary discovery reproduced native exit **139** before the fix and passed
**10/10** afterward with Python 3.14.7/Pillow 12.3.0/PyMuPDF 1.28.2. PDF rasterization,
page order and metadata executed. Logs `...-f6-red.log`, `...-f6-green.log` (same
`/private/tmp/crossink-manga-final-fix` prefix). The harmless upstream SWIG
`__module__` deprecation warning remains recorded; this is not a warning-free claim.
No device validation applies to this test-fixture lifetime correction.

## M1–M3 and static dispositions

M1: reviewed all public manga documents together. Updated converter setup/current
integration/PDF evidence; portable format fallback and reader-stage language;
canonical sparse/mixed priority, cancellation, legacy/TOC cursor documentation;
v6/v4 language statistics, retained completion edits, managed moves and checked
Recent deletion/recovery; full sleep fallback policy; and file-format current vs
historical v5 descriptions. `docs/manga-bitmap-pixels.md` and
`docs/manga-pixel-cache.md` required no change. Historical task reports were not
rewritten. CHANGELOG has five user-facing fixes only in Unreleased.

M2: removed only the five obsolete Nearby mirrors, their assignments and three
uncalled wrappers. `protocol_`, peer display/identity fields, `lastHelloMs_`,
`sendHello()`, callbacks and final-ACK retry remain. Relevant source is
`NearbyStatsSyncActivity.cpp:375,463,477` and its header. Existing nine
NearbyProtocol tests passed in the focused run. Pair-device updated/legacy
interoperability still requires hardware acceptance.

M3: `lib/GfxRenderer/BitmapHelpers.h:356` initializes `rowCount = 0`; successful
`begin()` and reset still clear it. Existing real-cover/codec coverage passed.

The final review already classifies all 51 unique static diagnostics. No global
suppression, typed-owner rewrite, cancellation-check deletion, baseline main/settings
warning cleanup or SDK change was made. M2/M3 are the only requested actionable
static cleanup. Root must rerun the configured static gate and retain nonzero
findings honestly; this fixer makes no static-clean or warning-free claim.

## Reproduction and final root gates

Focused native build and checks completed with exit 0:

```sh
cmake -S test -B build/task16-tests -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/private/tmp/crossink-manga-pixel-build/_deps/googletest-src
cmake --build build/task16-tests --target MangaBookTest MangaCoverTest RealCoverTest CompletionActionTest RecentDeleteTest ReadingLanguageStatsTest -j2
ctest --test-dir build/task16-tests -j1 --output-on-failure -R 'MangaBookTest|Cover|LanguageStatsTest|NearbyProtocol|RecentDeleteTest|CompletionActionTest|MangaSourceBoundaryTest|MangaMenuTest|MangaStatsCommitTest|MangaStatusTest|MangaStatusPixelsTest|MangaQrTest|QrPolicyTest|QrDrawTest|MangaBookmarkSave|MangaScreenshotWriteTest|MangaSmokeValidationTest'
/private/tmp/crossink-manga-pdf-validation/bin/python -B -m unittest discover -s test/manga_converter -v
```

Logs: `...-native-configure.log`, `...-focused-build.log`,
`...-focused-green.log` (**131/131**). The initial source-boundary CTest registration
was Not Run because Python was discovered only in a child CMake scope; the root
scope discovery is fixed and the final registered test passes all six cases.
The original thirteen menu regression cases and real QR/mono/gray targets are
preserved; no narrower count is substituted for the final whole-suite gate.

Root's required fresh gates after freeze:

```sh
cmake --build build/task16-tests -j2
ctest --test-dir build/task16-tests -j1 --output-on-failure
/private/tmp/crossink-manga-pdf-validation/bin/python -B -m unittest discover -s test/manga_converter -v
pio run -e default -e sticky -e x4-pro -e simulator -e sticky-simulator
python3 -B /private/tmp/crossink_final_upload_suspend.py --env simulator --mode home --port 23980
python3 -B /private/tmp/crossink_final_upload_suspend.py --env simulator --mode pop --port 23982
python3 -B /private/tmp/crossink_final_upload_suspend.py --env simulator --mode sleep --port 23984
python3 -B /private/tmp/crossink_final_upload_suspend.py --env sticky-simulator --mode home --port 23986
python3 -B /private/tmp/crossink_final_upload_suspend.py --env sticky-simulator --mode pop --port 23988
python3 -B /private/tmp/crossink_final_upload_suspend.py --env sticky-simulator --mode sleep --port 23990
```

Also run root's already-prepared 12-job normal/OCR/grayscale/prefetch/full-menu/
failure-retry/EPUB simulator matrix and configured changed-path cppcheck gate.
Record exact artifact hashes and commands in root's final evidence; no such
firmware/simulator run is claimed in this report. The endpoint, C3/S3 physical
acceptance and separately unapproved dictionary export/restore remain root-owned
external limits, with backup/fonts untouched.

## New-file inventory

- `test/source_contracts/extract_functions.py`
- `test/book_folder_mutation/RecentDeleteTest.cpp`
- `test/reading_language_stats/CompletionActionTest.cpp`
- `src/activities/home/BookCompletionEdit.h`
- `src/activities/home/BookCompletionActivity.h`
- `src/activities/home/BookCompletionActivity.cpp`
- `test/source_contracts/test_upload_suspension.py`
- `test/source_contracts/test_sleep_cover.py`
- `test/source_contracts/test_completion_lifecycle.py`
- `docs/superpowers/plans/2026-09-07-manga-final-fix-report.md`


## Final-gate stress assertion correction

The fresh gate's two stress runs completed the app flow but the Python validator rejected an obsolete `Deferring activity transition until prefetch files close` message. The task10e retained-statistics rewrite removed that debug statement while preserving `foregroundReadyLocked()`; the mismatch already exists in the exact pre-fix baseline. Cancellation is requested before `RenderLock`, so requiring an intermediate deferral log would also depend on worker scheduling.

The supplemental change is limited to `scripts/run_manga_simulator_smoke_test.py` and its existing `test/manga_menu/MangaSmokeValidationTest.py`, plus this report. Every other required coalescing, menu, child-pop, transition, refresh, and sleep marker remains required. A new ordered check requires a distinct held worker, then `Result::Cancelled`, then the expected activity entry/render for child push, refresh, replace, reader pop, and sleep. Replace/pop/sleep must also cancel before the old reader exits. Worker completion is published only after decoder/file owners return and cache files close. Existing crash/error, grayscale, progress, and library validators are unchanged.

Focused Python validation: **9/9 tests passed**, including rejection of each missing/reused hold or completion, a non-cancelled result, cancellation after activity entry, early reader exit, and missing retained lifecycle markers. Evidence: `/private/tmp/crossink-manga-final-stress-validator-green.log`. Both previous runtime transcripts pass the new ordered helper when their harness diagnostic headers are excluded; this is recorded-output validation, not a fresh simulator pass. Root owns the required fresh two-profile stress rerun and its downstream persistence checks. Firmware/native build results remain applicable because no C/C++ files changed.

Exact immediately preceding Python/report snapshots: `/private/tmp/crossink-manga-final-stress-harness-before/`. Supplemental diff: `/private/tmp/crossink-manga-final-stress-harness.patch`. No new repository paths, PIO runs, firmware changes, or hardware claims were added by this correction.
