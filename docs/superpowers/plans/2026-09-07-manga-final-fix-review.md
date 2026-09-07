# Manga final correction review — 2026-09-07

Status: complete. Specification PASS for the reviewed software; code quality PASS. Physical acceptance remains pending, and static analysis is explicitly not clean (exit 1). This is the one scoped rereview of the combined correction wave against F1–F6 and M1–M3 in `2026-09-07-manga-final-review.md`. Source remains frozen; this review writes only this report and runs no build, test, serial, source mutation, or Git mutation.

## Coverage log

- Read `/private/tmp/crossink-manga-final-scoped-review-brief.md` completely. Original whole-port report and its exhaustive original requirements/ruling reconciliation remain the baseline.
- Correction package `/private/tmp/crossink-manga-final-fix.patch`: lines 1–460, 461–860, 861–1310, and 1311–1760 read in bounded, untruncated outputs. No skipped or truncated source is counted as reviewed.
- The complete correction implementation report is included in the already reviewed patch range (285 added lines); its test claims are implementation evidence, separately identified from root final gates.
- Areas covered so far: all public documentation correction hunks; correction report; dither row initialization; MangaBook sparse/mixed canonical page identity and cooperative cancellation; MangaCover propagation; full sleep-cover path handoff; completion transaction function and the new activity owner through its header (header continues in next chunk).

## Initial root verification checkpoint (superseded by final gate results below)

- Fresh ordinary installed-PDF converter discovery: 10/10 pass, exit 0, 0.228 seconds; upstream SWIG deprecation retained. Log `/private/tmp/crossink-manga-final-verification-converter.log`.
- Fresh full native build: exit 0. Complete sequential CTest: 811/811 pass, exit 0, 66.08 seconds. Log `/private/tmp/crossink-manga-final-verification-native-tests.log`.
- Fresh C3 + simulator + sticky-simulator builds: pass, 117.922 seconds. Log `/private/tmp/crossink-manga-final-verification-build-c3-sims.log`. C3 binary 6,504,800 bytes, 48,800 bytes free; SHA-256 `62c638e467ff8ed5776c59ea310c0e498fd1dd080cd2eaf2004a83617e6fec91`.
- Remaining simulator, actual WebSocket lifecycle, S3 board builds, and static results pending. None establishes physical device acceptance.

- Correction patch lines 1761–2210, 2211–2660, 2661–3110 and 3111–3564 read in bounded, untruncated outputs. Complete 1–3564 diff-line coverage achieved; direct lifecycle/cleanup collaborator validation remains in progress. All new test source and CMake extraction dependencies, Recent list/grid wiring, Nearby deletions, upload cleanup and simulator trigger reviewed.

## Finding-by-finding assessment

The following assesses the completed correction source. Final gate status and final specification/quality verdicts are recorded separately below; no physical acceptance is inferred.

### F1 — addressed: uploads cannot retain a loop-dependent suspension latch

`CalibreConnectActivity.h:47` and `CrossPointWebServerActivity.h:89` now invoke `cancelActiveUploads()` from main-owner preparation before testing readiness. `CrossPointWebServer.cpp:418` closes an active WebSocket file through the existing abort policy and clears its ownership flag/client at `:402`. It closes HTTP and font files, drops buffered bytes, invalidates success/valid state, removes partials best-effort, and stops the active HTTP client. The hook does not send or flush WebSocket traffic. It leaves the server available for a pushed activity to return to; `stop()` reuses it at `:457`.

Direct collaborator review confirms why this resolves the original cycle: the pending-action loop at `ActivityManager.cpp:229` can complete preparation while ordinary activity input/socket pumping remains suppressed at `:190`. The existing manager calls preparation under RenderLock and main's sleep path uses the same contract. Late WS binary callbacks at `CrossPointWebServer.cpp:1950` reject cleared ownership before writing. HTTP WRITE/END at `:939`/`:967` require the file; font validity is cleared and its path erased, so later font END/ABORT cannot remove a recreated path. All reviewed file owners close explicitly. Physical SD close/remove calls remain cooperative and may be slow; partial removal failure does not retain an open handle. The existing normal Web Back restart path is unchanged.

The extracted-hook test exercises both production hook bodies but doubles cleanup, so it alone is not sufficient runtime integration evidence. Root's actual localhost WebSocket regression supplies START/READY and incomplete binary data, holds the socket open through managed Home/pop/sleep, demands partial-file removal and uses a four-second deadline below the simulator's five-second socket timeout. All six profile/action combinations passed in the actual Web Server activity; both activity hooks are covered by the extracted test and source review, while this runtime harness does not independently navigate through Calibre. For sleep the harness requires the post-preparation main marker as well as return, preventing a deferred-return false pass. Hardware still needs interruption of a disposable upload on C3 and S3, confirming navigation/sleep and partial cleanup without a held SD owner.

### F2 — addressed: sparse and mixed image families preserve physical page indexes

`MangaBook.cpp:332` recognizes a retained page-zero canonical image without requiring the last page. `:357` performs one bounded classification only when page zero is missing, accepting an exact in-range canonical numeric filename; `:483` probes `.jpg`, `.jpeg`, `.bmp`, `.png` in that deterministic order for each requested physical index. A hole returns Missing instead of compacting later full-page art onto the wrong OCR/progress index. Legacy dense enumeration is reserved for a family with no canonical identity. Complete canonical families avoid directory enumeration and use at most four direct probes per request. Missing-cover classification reuses the existing fallible three-filename scan owner, with explicit entry/directory closure and no new whole-directory filename vector.

`MangaBookTest.cpp:605` onwards adds literal four-page sparse index coverage with a retained cover and middle PNG, absent last image, panel crop fallback and original OCR bytes. Other cases cover cover-plus-crops, JPEG extension, duplicate priority, mixed middle images and missing-cover classification exactly once. The earlier test's incorrect whole-book extension assumption was changed to the required per-page priority. Paths and crop resolution run through real MangaBook code, not a duplicate selector. The firmware reader's existing Missing-to-panel-crop behavior was unchanged. Physical acceptance should use the same sparse/mixed disposable book on both device classes and compare visible art, panel/OCR alignment and reopen position.

### F3 — addressed: both Recent Books delete callbacks use the shared transaction

`RecentBooksActivity.cpp:202` and `RecentBooksGridActivity.cpp:527` now invoke `BookFolderMutation::remove()` for manga. Their Complete branch alone reloads the view; failure reports the existing translated general or recovery-pending message. They do not repeat raw content deletion, discard snapshot results, or independently clear selected metadata. The established mutation owner handles nested identities, resume references, bookmarks, recents, partial outcomes and durable recovery. Ordinary non-manga deletion keeps its prior behavior.

`RecentDeleteTest.cpp:104` onwards compiles the exact selected production callbacks, with dependency-tracked extraction in `test/book_folder_mutation/CMakeLists.txt:24`. Both list and grid variants link the real transaction/journal/JSON/bookmark mutation implementation. Eight cases cover successful resume cleanup, only-absent cleanup after partial physical deletion, metadata failure followed by recovery, and a recreated path retaining metadata/content. UI and classification are doubles; the result callback body and journal engine are real. The retained `lastSleepFromReader` boolean when `openEpubPath` is cleared is the established serialized-state policy, not a missing pathname cleanup. Device confirmation remains a disposable nested manga deleted from each Recent layout and reboot/recovery after a controlled storage failure.

### F4 — addressed: one activity owns the frozen completion edit until both targets save

`BookCompletionEdit.h:10` contains the two summaries, metadata and `ReadingStatsEditState`. `BookActions.cpp:207` initializes them once after successful readable loads; `:246` freezes the desired completion value/date and count change. Later attempts at `:265` preserve the saved-target mask and write only missing targets. `:266` guards recents/move effects so they execute once after persistence completes. This retains the accepted per-file publication model and adds no durable cross-file journal.

The fallibly allocated activity at `BookActions.cpp:301` owns this state on the heap, rather than adding the summaries to the C3 task stack. Its `BookCompletionActivity.h:23` suspension hook refuses a dirty edit, `:24` cancels failed transition intent so ordinary input returns, and `:25` prevents automatic sleep. Global input/home gestures are blocked. `BookCompletionActivity.cpp:30` only retries from the selected Retry action; dismissing a dirty popup reopens it without writing, and onExit has no hidden save. A failure before initialization has no partially published edit and can be dismissed. Direct manager review confirms onEnter and result callbacks run with the manager's lock released before acquiring their own RenderLock (`ActivityManager.cpp:282`, `:318`); this modal does not introduce recursive-lock deadlock. Actual OptionPopup cancellation returns handled for the cancelling event then inactive on the next loop, which correctly preserves or dismisses the owner according to dirtiness.

All four call sites (FileBrowser manga/ordinary and Recent list/grid) now use `startCompletionEdit()`. Native CompletionAction tests extract the real completion function and use real book/global serializers with failed writes in both directions, persistent failure, changed clock and successful explicit retry. They prove count 7 becomes 8 once, a frozen date stays unchanged, recents effects run once and repeated completed calls do no writes. Metadata/type recognition and move collaborators are doubles, so they establish the common transaction rather than a full UI/EPUB move integration. The lifecycle test compiles real activity methods/hooks against a popup/input/save double and verifies failed dismissal, repeated suspension and explicit retry without hidden saves. Device validation should fault a disposable completion save, then verify the retained Retry UI and exact book/global result after recovery.

### F5 — addressed: sleep fallback does not restart source work after cancellation

`SleepActivity.cpp:733` checks the attempt token before source work and `:741` receives the validated/published cache path from preparation. On cancellation/failure it uses the selected default/custom fallback, without invoking `cachedCoverPathFor()` again for manga. `SleepCoverAssets.cpp:101` passes the same token through MangaBook open, discovery and dimensions; `:117` returns a cache path only for Cached/Published. Dimensions polling at `:73`, `:80`, `:96` brackets source lookup/codec work. `MangaBook.cpp:105`, `:332`, `:357`, `:425`, `:483` polls index records, directory enumeration and direct image probes, closes on failure and allows a clean retry. `MangaCover.cpp:352` propagates the same token to source path resolution. The separate cached-cover utility accepts an optional token for other callers; the cancelled full-sleep path no longer calls it. Explicit cache bitmap closure was also added at `SleepActivity.cpp:762`/`:767`.

The budget clock starts on the first `cancellation()` request (`SleepCoverBudget.h:27`), so the early check does not newly count the preceding sleep-entry popup as elapsed budget. Individual HAL/codec/close operations are still cooperative, not preemptible or hard-time-bounded. Reader/Metadata modes continue their ordinary metadata/TOC work; sleep uses Cover mode and bypasses that work.

Four source-extracted integration tests compile the actual sleep render/preparation functions and count complete source passes, dimension probes and fallback renders for expired-start, discovery expiry, conversion expiry and success without a second pass. Source/codec collaborators are doubles, complemented by real MangaBook tests cancelling mid-index and after two directory entries with zero retained handles and successful later retry. This correctly separates integration control-flow evidence from actual codec and physical timing evidence. C3/S3 slow-storage elapsed/max-poll-gap and watchdog behavior remain an explicit device gate.

### F6 — addressed: optional module guards restore only their owned keys

`test/manga_converter/test_converter.py:46` replaces the whole-module-table patch with per-key sentinel/restore cleanup for `huggingface_hub` and `ultralytics`. Default-argument binding preserves each name/prior value; unrelated imports, including PyMuPDF's native module owners, remain loaded. Socket/subprocess/environment guards and the installed-PDF test remain enabled; no preimport or skip masks the original native cleanup defect. Root's fresh ordinary discovery with the isolated Python 3.14.7 / Pillow 12.3.0 / PyMuPDF 1.28.2 environment passed all ten tests, exit 0. The upstream SWIG deprecation is retained and is not the prior cleanup crash.

### M1 — addressed: current public documents match implemented behavior

The complete public-document diff was reviewed together. `docs/manga-storage.md:59` documents sparse/mixed canonical identity, JPEG and per-page duplicate priority; `docs/manga-progress.md:41` documents v6/v4, language/day bounds, retained completion edits and shared Recent deletion; `docs/file-formats.md:312` and `:815` identify current and legacy records. `docs/manga-covers.md:18` documents the prepared-path handoff and preserves the unmeasured 2500 ms policy caveat. `docs/manga-format.md` removes future-reader claims, preserves optional malformed-data/fallback behavior and states physical acceptance separately. `tools/manga_convert/README.md:138` records ordinary installed-PDF validation and still distinguishes untested live YOLO/cloud work and pending physical acceptance. CHANGELOG describes the concrete corrected user behavior. Historical task reports were not rewritten as fresh passes.

### M2 — addressed: only dead Nearby mirrors/wrappers were removed

The five obsolete members and their assignments are deleted, together with the uncalled `sendDeviceName`, `sendLocalStats`, `sendAck` wrappers. `NearbyStatsSyncActivity.cpp:283` still starts the protocol, `:361` retains real callbacks, `:375` maps protocol state, and `:463` retains hello timing. Live peer identity/display fields, lastHelloMs, protocol retries, durable-save-before-ACK and final ACK behavior are untouched. Existing protocol tests and the fresh full native gate supply regression evidence; actual peer-device exchange remains a physical gate.

### M3 — addressed: deterministic default dither state

`BitmapHelpers.h:356` now initializes `rowCount = 0`, while successful `begin()` and reset still reset it. No codec allocation/error semantics changed. Existing real codec/dither coverage is appropriate; an implementation-mirroring test was not added.

## Initial gate discrepancy (resolved by the reviewed supplement below)

Fresh stress runs for both simulator profiles reached `Simulator smoke test passed`, but the Python wrapper exited 2 because `scripts/run_manga_simulator_smoke_test.py:435` still demands `Deferring activity transition until prefetch files close`. That phrase is absent from the current production transition implementation; actual hold/coalescing/push/pop/replace/main-sleep markers remain in the output. This is an existing stale verification assertion, outside the 44-path correction diff, not evidence that these corrections reintroduced a lifecycle defect. It still prevents calling the wrapper gate passed: assertions after this check, including final progress/library validation, must also complete. The accepted Python-only supplement and fresh reruns below resolve this narrow verification discrepancy. This reviewer did not launch another fixer.

## Exact correction coverage and integrity

The immutable correction package has 3,564 lines across 44 paths (34 existing, ten new), SHA-256 `cfe2c1b433198012894b1ea850dab9cd68416af9711506c65811b4ec9358ffb7`. All diff lines were read, including document/report additions and every new test. No new binary fixtures are in this correction package; the original 13 binary fixtures remain covered by the whole-port review. All 316 source/test/public-document paths in root’s frozen manifest were independently rehashed during this rereview: zero mismatches at this checkpoint. This reviewer changed only this report.

| Correction path | Diff lines reviewed | Status |
| --- | --- | --- |
| `CHANGELOG.md` | 2–30 | Complete |
| `docs/file-formats.md` | 31–107 | Complete |
| `docs/manga-covers.md` | 108–128 | Complete |
| `docs/manga-format.md` | 129–216 | Complete |
| `docs/manga-progress.md` | 217–301 | Complete |
| `docs/manga-storage.md` | 302–455 | Complete |
| `docs/superpowers/plans/2026-09-07-manga-final-fix-report.md` | 456–743 | Complete |
| `lib/GfxRenderer/BitmapHelpers.h` | 744–763 | Complete |
| `lib/MangaPanel/MangaBook.cpp` | 764–1048 | Complete |
| `lib/MangaPanel/MangaBook.h` | 1049–1114 | Complete |
| `lib/MangaPanel/MangaCover.cpp` | 1115–1139 | Complete |
| `src/activities/boot_sleep/SleepActivity.cpp` | 1140–1200 | Complete |
| `src/activities/boot_sleep/SleepCoverAssets.cpp` | 1201–1387 | Complete |
| `src/activities/boot_sleep/SleepCoverAssets.h` | 1388–1417 | Complete |
| `src/activities/home/BookActions.cpp` | 1418–1638 | Complete |
| `src/activities/home/BookActions.h` | 1639–1678 | Complete |
| `src/activities/home/BookCompletionActivity.cpp` | 1679–1732 | Complete |
| `src/activities/home/BookCompletionActivity.h` | 1733–1763 | Complete |
| `src/activities/home/BookCompletionEdit.h` | 1764–1784 | Complete |
| `src/activities/home/FileBrowserActivity.cpp` | 1785–1876 | Complete |
| `src/activities/home/RecentBooksActivity.cpp` | 1877–2000 | Complete |
| `src/activities/home/RecentBooksGridActivity.cpp` | 2001–2123 | Complete |
| `src/activities/network/CalibreConnectActivity.h` | 2124–2146 | Complete |
| `src/activities/network/CrossPointWebServerActivity.h` | 2147–2169 | Complete |
| `src/activities/network/NearbyStatsSyncActivity.cpp` | 2170–2303 | Complete |
| `src/activities/network/NearbyStatsSyncActivity.h` | 2304–2352 | Complete |
| `src/network/CrossPointWebServer.cpp` | 2353–2419 | Complete |
| `src/network/CrossPointWebServer.h` | 2420–2445 | Complete |
| `src/simulator/SimulatorSmokeTest.cpp` | 2446–2485 | Complete |
| `test/CMakeLists.txt` | 2486–2523 | Complete |
| `test/book_folder_mutation/CMakeLists.txt` | 2524–2559 | Complete |
| `test/book_folder_mutation/RecentDeleteTest.cpp` | 2560–2706 | Complete |
| `test/manga_book/MangaBookTest.cpp` | 2707–2852 | Complete |
| `test/manga_book/stubs/HalStorage.cpp` | 2853–2915 | Complete |
| `test/manga_book/stubs/HalStorage.h` | 2916–2938 | Complete |
| `test/manga_converter/test_converter.py` | 2939–2977 | Complete |
| `test/reading_language_stats/CMakeLists.txt` | 2978–3003 | Complete |
| `test/reading_language_stats/CompletionActionTest.cpp` | 3004–3137 | Complete |
| `test/reading_language_stats/stubs/HalClock.h` | 3138–3157 | Complete |
| `test/source_contracts/extract_functions.py` | 3158–3189 | Complete |
| `test/source_contracts/test_completion_lifecycle.py` | 3190–3279 | Complete |
| `test/source_contracts/test_sleep_cover.py` | 3280–3420 | Complete |
| `test/source_contracts/test_upload_suspension.py` | 3421–3473 | Complete |
| `tools/manga_convert/README.md` | 3474–3564 | Complete |

Patch line 1 is package framing and was included in the initial read. The ten-new-path inventory was reconciled individually against the table; no inventory path is omitted.

## Validation limits retained from the whole-port review

- Fresh root results do not convert the earlier X4 “all good” checkpoint into acceptance of the current firmware. Current binaries are unflashed. Physical C3/S3 manga sparse/mixed ordering, e-ink grayscale/ghosting, rotation/reopen, slow SD cancellation and poll gaps, internal heap/PSRAM behavior, render-task QR stack headroom, Nearby peer exchange, dictionary latency and SD font cycling remain device checks.
- No serial, reader data export, cloud/OCR call, user-image upload, model-weight download, dictionary operation, Git mutation or SDK mutation was performed by this reviewer. The original full-review dictionary backup/export restrictions and unchanged SDK integration remain binding.
- The new native source-boundary tests intentionally double platform/UI/metadata collaborators. Their production bodies are extracted verbatim with rebuild dependencies; they are useful failure-path tests, not a claim that the complete UI or hardware runs natively. Real firmware simulator flows, real codecs, journal/stats stores and the installed-PDF converter gate provide complementary evidence.
- Native validation here is macOS. The new CompletionAction fixture follows existing reading-stats fixtures in using `/private/tmp` (`CompletionActionTest.cpp:68`); this evidence does not establish Linux/Windows fixture-path portability. No new supported-platform regression was inferred from an already platform-specific fixture group.
- Complete original handoff/ledger reconciliation remains in `2026-09-07-manga-final-review.md`; the corrections close the F1–F6/M1–M3 gaps cited there. They do not silently promote its physical-only or optional external-service items to completed checks.

## Root final gate evidence received during rereview

| Gate | Fresh result | Evidence / limitation |
| --- | --- | --- |
| Full native build and sequential CTest | Build exit 0; 811/811 pass, 66.08 seconds | `/private/tmp/crossink-manga-final-verification-native-tests.log`; before the supplemental Python stress-harness assertion correction |
| Ordinary installed-PDF converter discovery | 10/10 pass, exit 0, 0.228 seconds | `/private/tmp/crossink-manga-final-verification-converter.log`; reviewer read all ten results and the retained SWIG deprecation |
| C3, simulator, sticky-simulator builds | All pass, 117.922 seconds | `/private/tmp/crossink-manga-final-verification-build-c3-sims.log` |
| Sticky firmware | Pass, 221.217 seconds | `/private/tmp/crossink-manga-final-verification-build-s3.log` |
| X4 Pro firmware | Pass, 122.302 seconds | Same S3 log; total both builds 343.519 seconds |
| Real WS managed Home/pop/sleep | 6/6 pass, exit 0 | Reviewer read `/private/tmp/crossink-manga-final-verification/upload-results.json`, all six individual result logs and C3 activity/sleep trace excerpts. Transition + partial cleanup: simulator Home 0.379 s, pop 0.380 s, sleep 0.645 s; sticky-simulator Home 0.377 s, pop 0.377 s, sleep 0.655 s. Socket held open until completion. |
| Normal/OCR/menu-gray/failure-retry/EPUB simulator flows | 10/10 pass, root-reported | Both simulator profiles; stress wrapper results tracked separately below |
| Stress wrappers | Corrected fresh reruns 2/2 pass, exit 0 | 35.205 s / 35.304 s; both downstream persistence/library checks execute successfully. Initial failed transcripts remain preserved. |
| Static analysis | Valid execution, exit 1; not clean | 162 selected C/C++ paths, 33.238 s, 0 high / 6 medium / 57 low reports, 60 unique diagnostics. Full disposition below; no unsafe defect found. |

The actual Home/pop Web Server traces show the existing onExit silent restart after managed preparation. The partial is deleted before activity exit/restart. These runs therefore exercise the readiness gate; they do not merely rely on later disconnect cleanup. Sleep separately reaches the main post-preparation marker.

## Supplemental stress-harness correction — accepted

Read all 174 lines of `/private/tmp/crossink-manga-final-stress-harness.patch` in one bounded, untruncated output, including the two existing Python files and correction-report appendix. No firmware source was changed. Root authorized this narrow gate correction after confirming the obsolete assertion existed in the exact pre-fix baseline.

`validate_prefetch_stress()` in `scripts/run_manga_simulator_smoke_test.py:47` retains all the other required lifecycle markers. It additionally requires a distinct hold after the previous phase, then `Prefetch result=4`, then the expected target entry/render for child push, manual refresh, replace, reader pop and main sleep. Replace/pop/sleep also require cancellation before the reader exits. A single earlier hold/result cannot satisfy later phases. This tests actual readiness ordering without depending on whether scheduling exposes an intermediate deferral log. Existing crash/error detection, grayscale checks, and final progress/library checks remain in the wrapper.

The signal is meaningful: `MangaPrefetch.h:34` defines Cancelled as enum value 4. `MangaPrefetch.cpp:152` returns from produce, then closes/discards cache files at `:154`, and publishes completion at `:157`; foreground `poll()` logs the result only after consuming completion at `:134`. Therefore requiring that log before target entry/old-reader exit proves the tested worker returned its file ownership.

All six new focused methods in `MangaSmokeValidationTest.py:49` were reviewed: valid transcript without old log, each missing/reused hold or completion, non-cancelled result, cancellation after entry, early old-reader exit, and missing retained lifecycle markers. Root reports all nine methods (three existing plus six new) pass. These are appropriate tests of the changed validator; they are not substituted for fresh actual stress reruns.

Supplemental patch SHA-256: `4830d314a148c3b8f714f7dc03552d191fcd1edc249ccfe348258ee86aedd694`.

| Supplemental path | Diff lines reviewed | Status |
| --- | --- | --- |
| `scripts/run_manga_simulator_smoke_test.py` | 1–77 | Complete |
| `test/manga_menu/MangaSmokeValidationTest.py` | 78–157 | Complete |
| `docs/superpowers/plans/2026-09-07-manga-final-fix-report.md` | 158–174 | Complete |

Frozen-manifest comparison after this authorized supplement differs only at `scripts/run_manga_simulator_smoke_test.py`, `test/manga_menu/MangaSmokeValidationTest.py`. These are the two reviewed Python paths. The correction-report appendix is outside the source/public-document frozen manifest. All other frozen paths still match.

## Final static analysis — complete triage, command remains non-clean

Root's fresh cppcheck/PIO check executed successfully across 162 selected changed/new C/C++ paths in 33.238 seconds and returned **exit 1**, with **0 high / 6 medium / 57 low reports (60 unique location/diagnostics)**. The command is not presented as a clean gate. Exact evidence: `/private/tmp/crossink-manga-final-verification-static.log` and `/private/tmp/crossink-manga-final-verification-static-findings.txt`; the reviewer read the complete 60-line deduplicated output and compared it with the original 51-line evidence.

The M3 uninitialized-row-count diagnostic is gone. Fifty old location/diagnostics remain (one MangaBook path helper line shifted from 83 to 84), with ten newly surfaced locations. All six Medium reports are repeated cancellation conditions. `CooperativeCancellation.h:9` invokes a borrowed callback/context; const does not mean the callback or elapsed deadline cannot change. Removing these checks would weaken the corrected cancellation boundaries. No blanket suppression, typed-array rewrite or unrelated compiler-warning cleanup is warranted.

The ten new locations were inspected in current source. The remaining source-backed classifications from the full review are carried individually below; frozen hashes establish that their source did not change except the separately reviewed MangaBook/cancellation adjustments. Outside-menu main narrowing and Settings switch warnings remain the original unchanged compiler-warning limitations; they were not hidden or included as successful static-clean claims.

| Current location | Severity / diagnostic | Disposition |
| --- | --- | --- |
| `lib/Dict/DictIndex.cpp:760` | low:style / `constParameterReference` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `lib/Dict/DictIndex.cpp:227` | low:style / `useStlAlgorithm` | Optional style suggestion; explicit bounded/early-exit loops are clear, no correctness finding. |
| `lib/Dict/DictIndex.cpp:605` | low:style / `useStlAlgorithm` | Optional style suggestion; explicit bounded/early-exit loops are clear, no correctness finding. |
| `lib/Dict/DictIndex.cpp:212` | low:portability / `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |
| `lib/Dict/DictIndex.cpp:375` | low:portability / `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |
| `lib/Dict/DictIndex.cpp:703` | low:portability / `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |
| `lib/Dict/DictIndex.cpp:706` | low:portability / `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |
| `lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp:538` | low:style / `knownConditionTrueFalse` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/Epub/Epub/converters/DirectPixelWriter.h:36` | low:style / `constParameterReference` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `lib/Epub/Epub/converters/PngToFramebufferConverter.cpp:474` | low:style / `knownConditionTrueFalse` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/Epub/Epub/converters/PngToFramebufferConverter.cpp:168` | low:style / `constVariablePointer` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `lib/Epub/Epub/converters/PngToFramebufferConverter.cpp:176` | low:style / `constVariablePointer` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `lib/JpegToBmpConverter/JpegToBmpConverter.cpp:116` | low:style / `constVariable` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `lib/JpegToBmpConverter/JpegToBmpConverter.cpp:154` | low:style / `constVariable` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `lib/MangaPanel/MangaBook.cpp:110` | medium:warning / `identicalConditionAfterEarlyExit` | Required repeated borrowed-callback/deadline poll after existence I/O; requested() const is not pure. No defect; retain. |
| `lib/MangaPanel/MangaBook.cpp:114` | low:style / `knownConditionTrueFalse` | Required cancellation recheck after data-file I/O/close and before opening index. Callback/time may change; retain. |
| `lib/MangaPanel/MangaBook.cpp:358` | low:style / `variableScope` | Optional narrower scope for 32-byte name scratch; bounded current lifetime introduces no correctness or memory defect. |
| `lib/MangaPanel/MangaBook.cpp:359` | low:style / `variableScope` | Optional narrower lexical scope for static constexpr extension table; no allocation/lifetime defect. |
| `lib/MangaPanel/MangaBook.cpp:84` | low:portability / `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |
| `lib/MangaPanel/MangaCover.cpp:172` | medium:warning / `identicalConditionAfterEarlyExit` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/MangaPanel/MangaCover.cpp:348` | medium:warning / `identicalConditionAfterEarlyExit` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/MangaPanel/MangaCover.cpp:383` | medium:warning / `identicalConditionAfterEarlyExit` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/MangaPanel/MangaCover.cpp:399` | medium:warning / `identicalConditionAfterEarlyExit` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/MangaPanel/MangaCover.cpp:259` | low:style / `knownConditionTrueFalse` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/MangaPanel/MangaCover.cpp:356` | low:style / `knownConditionTrueFalse` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/MangaPanel/MangaCover.cpp:434` | low:style / `knownConditionTrueFalse` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/MangaPanel/MangaPixelCache.cpp:169` | medium:warning / `identicalConditionAfterEarlyExit` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/PngToBmpConverter/PngToBmpConverter.cpp:150` | low:style / `constVariable` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `lib/PngToBmpConverter/PngToBmpConverter.cpp:179` | low:style / `constVariable` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `lib/hal/HalStorage.cpp:118` | low:performance / `useInitializationList` | Existing internal HAL construction style; no introduced functional issue or required rewrite. |
| `lib/hal/HalStorage.cpp:233` | low:style / `noExplicitConstructor` | Existing internal HAL construction style; no introduced functional issue or required rewrite. |
| `lib/hal/HalStorage.cpp:24` | low:style / `unusedStructMember` | Default-profile-only view; massStorage is used by capability-gated S3 USB paths, preserve. |
| `src/activities/ActivityManager.cpp:402` | low:style / `useStlAlgorithm` | Existing bounded activity-stack reader rejection loop; std::all_of/none_of is optional style. |
| `src/activities/boot_sleep/SleepCoverAssets.cpp:96` | low:style / `knownConditionTrueFalse` | Necessary post-dimension/codec cancellation poll before further work. Callback/time may change during codec I/O; retain. |
| `src/activities/boot_sleep/SleepCoverAssets.cpp:93` | low:style / `constVariablePointer` | Optional pointer-to-const spelling for dimension decoder; no mutation/lifetime defect. |
| `src/activities/reader/BookStatsView.cpp:763` | low:style / `useStlAlgorithm` | Existing fixed eight-language-entry count loop; count_if is optional style. |
| `src/activities/reader/MangaPageTextSource.cpp:151` | low:performance / `passedByValue` | Small borrowed PageView value, bounded; optional const-reference style, no demonstrated performance defect. |
| `src/activities/reader/MangaPageTextSource.cpp:263` | low:performance / `passedByValue` | Small borrowed PageView value, bounded; optional const-reference style, no demonstrated performance defect. |
| `src/activities/reader/MangaProgressStore.cpp:82` | low:performance / `useInitializationList` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `src/activities/reader/MangaQrPayload.cpp:75` | low:portability / `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |
| `src/activities/reader/MangaReaderActivity.cpp:802` | low:style / `duplicateCondition` | Same locked value intentionally cancels auto-turn then chooses pause versus resume. Both true-branch effects are required; no duplicate side effect or lost else. |
| `src/activities/reader/MangaTranslationActivity.h:11` | low:performance / `passedByValue` | Small borrowed PageView copy is retained by the child while parent bytes remain immutable/alive. Optional const-reference parameter does not fix a functional defect. |
| `src/activities/reader/ReadingLanguageStats.cpp:275` | low:style / `useStlAlgorithm` | Optional style suggestion; explicit bounded/early-exit loops are clear, no correctness finding. |
| `src/util/BookDeletionSnapshot.cpp:78` | low:style / `shadowFunction` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `src/util/BookDeletionSnapshot.cpp:103` | low:style / `shadowVariable` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `src/util/BookFolderMutation.cpp:619` | low:style / `shadowFunction` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `src/util/BookFolderMutation.cpp:627` | low:style / `shadowFunction` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `src/util/BookFolderMutation.cpp:640` | low:style / `shadowFunction` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `src/util/BookFolderMutation.cpp:710` | low:style / `shadowFunction` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `src/util/BookFolderMutation.cpp:361` | low:style / `clarifyCalculation` | Precedence matches intended bit-test ternary; optional parentheses, not a correctness defect. |
| `src/util/BookFolderMutation.cpp:277` | low:style / `useStlAlgorithm` | Optional style suggestion; explicit bounded/early-exit loops are clear, no correctness finding. |
| `src/util/BookFolderMutation.cpp:298` | low:style / `useStlAlgorithm` | Optional style suggestion; explicit bounded/early-exit loops are clear, no correctness finding. |
| `src/util/BookFolderMutation.cpp:864` | low:style / `useStlAlgorithm` | Optional style suggestion; explicit bounded/early-exit loops are clear, no correctness finding. |
| `src/util/BookMutationJson.cpp:84` | low:style / `redundantCondition` | Redundant EOF/control-byte rejection is harmless; optional simplification only. |
| `src/util/BookMutationJson.cpp:89` | low:style / `shadowFunction` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `src/util/BookMutationStorage.cpp:204` | low:portability / `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |
| `src/util/BookMutationStorage.cpp:211` | low:portability / `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |
| `src/util/JapaneseDictionaryBackend.h:28` | low:style / `knownConditionTrueFalse` | Defensive cancellation recheck after backend call; no functional defect established, retain unless proven unnecessary. |
| `src/util/DictionaryEngine.cpp:390` | low:style / `knownConditionTrueFalse` | Defensive cancellation recheck after backend call; no functional defect established, retain unless proven unnecessary. |
| `src/util/StarDictBackend.cpp:393` | low:portability / `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |

## Final verdicts and completion

**Specification verdict: PASS for the reviewed software implementation and correction scope.** F1, F2, F3, F4, F5, F6, M1, M2 and M3 are all addressed. The original handoff/ledger gaps attributed to those findings are closed. The 174-line supplemental stress-validator correction is also accepted. No residual or correction-introduced Critical or Important defect was identified, and no new code fix is requested. Physical acceptance and previously explicit external-service/data-export constraints remain pending, rather than silently counted as implemented validation.

**Code-quality verdict: PASS.** The fixes preserve main-owner lifecycle/storage coordination, bounded/fallible manga memory ownership, saved-target acknowledgments, translation strings, dictionary integration and accepted Task10e input/menu behavior. Native failures are tested through real mutation/statistics implementations and exact production integration bodies; actual simulator WebSocket and reader flows complement the deliberate doubles. The final static command remains exit 1 with the complete source-backed dispositions above, not a static-clean pass.

Final fresh evidence is: **811/811 full native tests**, **10/10 ordinary installed-PDF converter tests**, successful **C3, Sticky, X4 Pro, simulator and sticky-simulator builds**, **12/12 logical reader/app simulator jobs**, and **6/6 real WS managed lifecycle jobs**. The full native run preceded the two-file Python-only supplement; its affected registered CTest subsequently passed **1/1**, and the focused validator passed **9/9 methods**. Firmware/C++ did not change, so the accepted firmware build/native results remain applicable. Both actual stress reruns exited 0 in 35.205/35.304 seconds and reached the wrapper's persisted-progress and grayscale confirmations; the stale-marker failure is resolved without claiming its original exit-2 runs passed. Evidence: `/private/tmp/crossink-manga-final-verification/stress-rerun-results.json`, `simulator-stress-rerun.log`, and `sticky-simulator-stress-rerun.log`, inspected by this reviewer.

At finalization, the reviewer independently rehashed `/private/tmp/crossink-manga-final-accepted-hashes.json`: **316/316 paths match**, zero discrepancies. That manifest includes precisely the two authorized Python changes relative to the earlier frozen manifest; no firmware source drift occurred. Root captured matching final C3 binary/ELF artifacts at `/private/tmp/crossink-manga-final-firmware.*` and reports a clean `git diff --check`. The reader endpoint remains absent; no final firmware flash or physical check is claimed. The separately unapproved dictionary export remains unperformed and requires the user's explicit authorization under the existing session restriction.

Review coverage is complete: **3,564 original correction diff lines / 44 paths, all ten new files, 174 supplemental diff lines / three existing paths, all nine original finding dispositions, all 60 final static diagnostics, and relevant direct lifecycle/worker/cancellation collaborators**. Earlier CodeGraph output used for locating Home helpers was truncated; no omitted source was counted as reviewed, and the relevant `ActivityManager.cpp:357`/`:612` helpers and actual WS traces were read separately in bounded ranges. No reviewer test/build/serial/subagent/Git action or source edit occurred; only this report was written.
