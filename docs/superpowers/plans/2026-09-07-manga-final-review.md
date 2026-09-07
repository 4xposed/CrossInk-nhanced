# Final whole-port manga review

Complete source review; findings require one combined fix wave. Read-only review of the complete
uncommitted working tree relative to `ea03940023c1705fed80ea64cc0de3208f38321a`
on `matcha_features`. Only this report is edited. No staging, commits, pushes,
SDK changes, PIO, serial, cloud calls, exports, or subagents by this reviewer.

## Persistent coverage log

- Read dispatch brief, reviewer rubric, AGENTS.md, `.claude/CONTEXT.md`, original
  handoff (all seven sections), complete library-completion plan, final evidence
  index, and known-findings file. Read all 630 lines of the SDD progress ledger,
  including every Ruling/deferred/parked disposition; reconciliation follows below.
- CodeGraph was consulted before source discovery. Its broad output was truncated
  and is navigation evidence only. Direct patch reads are the coverage authority.
- Manifest: 363 files: implementation 170, tests 128, documentation 9, planning 55,
  baseline index 1. Thirteen binary fixtures require separate contract/provenance
  inspection. The manifest records the unchanged SDK pin
  `1e8ee543edca397f2b8747811f5a88f1bc35d233`.
- Implementation patch reviewed through lines **1–3700**. Bounded reads: 1–800,
  801–1800, 1801–2900, 2901–3700. The outer tool truncated part of 1801–2900;
  the missing MangaBitmapPixels area was reread completely at 2150–2460.
  Coverage includes CooperativeCancellation/CheckedPrint, dictionary identity
  interfaces, decoder/cache changes, dither allocation changes, translations,
  JPEG cover conversion, MangaBitmapPixels, MangaBook, MangaCover, MangaFormat,
  and the beginning of MangaImageGeometry. `Memory.h` confirms array helpers
  value-initialize buffers, preserving removed explicit zero fills.

## Investigation notes (not findings until corroborated)

- Verify legacy directory enumeration error handling versus the new checked HAL
  API and pinned fallback ordering. It currently tests allocation failure only.
- Verify BMP cover constructor defaults and bounds before judging its dynamic
  scratch sizing. Cover output identity binds exact emitted dimensions and CRC;
  recovery recognizes all crossed primary/backup pairs.
- Large optional metadata/TOC buffers are explicitly fallible; assess maximum
  retained footprint alongside reader initialization and codec budgets.
- Strict candidate identity hashes indexes plus data extents, under immutable
  active-dictionary contract; compare expected identity semantics with scan tests.

## Final findings and verdicts

See the completed assessment below; earlier investigation notes are historical.

## Original handoff checkbox reconciliation

All 50 individual items are reconciled in the completed assessment below.

## Ledger disposition reconciliation

Every recorded ruling and parked item is reconciled below.

### Progress checkpoint 2

Implementation patch now reviewed through **1–9800**. Additional reads:
3701–4500, 4501–5300, 5301–6100, 6101–6800, 6801–7500,
7501–8300, 8301–9100, 9101–9800. The 5301–6100 read lost a small
middle segment to the inner cap; 5590–5660 reread covers it fully.
Coverage now includes all MangaPanel core code, PNG cover hardening, checked HAL,
font pooling/build scripts, complete Python simulator harness, bookmarks/state/
recents, ActivityManager suspension, sleep covers/budget, browser/Home/Recent
integration, and Nearby protocol/activity extraction. BookReadingStats is partial.

Additional investigation targets:

- New upload `prepareToSuspend()` hooks can remain false while ActivityManager
  suppresses `currentActivity->loop()` whenever a transition is pending. Verify
  whether HTTP/WebSocket uploads require that loop to finish; likely navigation
  deadlock if Back/Home arrives while an upload is active.
- Both Recent Books deletion handlers still use raw `Storage.removeDir` followed
  by best-effort metadata cleanup, whereas FileBrowser now uses journalled
  `BookFolderMutation::remove`. Check partial-deletion and resume-state contracts.
- `BookActions::toggleBookCompleted` saves book/global targets with bounded retries
  but discards per-target outcome on failure. Assess persisted half-success and
  next-toggle behavior against accepted per-file transaction limitations.
- Root static-check input arrived: cppcheck execution valid, exit 1, 0 high /
  14 medium / 46 low reports in `/private/tmp/crossink-manga-final-static.log`.
  Triage pending (repeated cancellation checks may be intentional volatile
  callbacks; `FloydSteinbergDitherer::rowCount` currently initialized by begin).

### Progress checkpoint 3

Implementation reviewed through **1–14700**, additionally reading 9801–10500,
10501–11200, 11201–11900, 11901–12600, 12601–13300, 13301–14000,
14001–14700 without truncation. Covers Book/Global statistics, EPUB span commits,
shared OCR/dictionary candidate identity, MangaPrefetch/navigation, and the Manga
reader through render and the beginning of prepareToSuspend. Remaining implementation
starts at 14701. Tests, documentation, planning patch and binary fixtures still pending.
Potential sleep-budget concern: fallback cover-path discovery may reopen/probe the
source after a cancellation deadline; inspect call graph before judging.

### Progress checkpoint 4

Implementation patch **1–24706 complete**. Additional bounded ranges:14701–15400,
15401–16100,16101–16800,16801–17500,17501–18200,18201–18850,18851–19350,
19351–19950,19951–20650,20651–21350,21351–22050,22051–22750,22751–23450,
23451–24100,24101–24706. The18201–18850 smoke-script read was truncated;
18510–18620 repaired the complete omitted segment. Final coverage includes all
170 implementation entries, including untracked source, converter, simulator
consumer patch and manifest, transaction engine, journal and owner/storage helpers.
Root is independently probing only active-upload transition liveness. Further
inspection target: removeBookFileMetadata deletes EPUB/Anki caches but does not
remove TXT/XTC caches after deletion; compare baseline contract and tests.

### Progress checkpoint 5

Tests patch reviewed through **1–5200**, in650-line contiguous bounded chunks,
without truncation. Coverage includes transaction/snapshot/JSON/journal/checked
EOF tests and stubs; font pooling byte equivalence; real image codec goldens;
all945 appended dictionary/OCR/identity tests and modified stub; BMP pixel tests;
MangaBook contract/failure tests; all converter tests; cover tests through most
of RealCoverTest. Next test chunk starts5201. The TXT/XTC cache retention concern
is explicitly asserted as existing policy by OrdinaryDeleteRetainsExistingXtcTxtCachePolicy;
it is not yet a finding. Candidate identity tests verify same-size body changes
preserve candidates while selected definitions use new content, confirming intent.

### Progress checkpoint 6

Implementation **24706/24706**, tests **9603/9603**, and documentation
**860/860** lines complete. Additional test chunks:5201–5850,5851–6500,
6501–7150,7151–7800,7801–8450,8451–9100,9101–9603; documentation1–440,441–860.
All returned without truncation. Covers final real-cover fault matrix, original
format/translation fixtures, geometry, all menu/status/QR/screenshot/bookmark
contracts, retained statistics transactions, actual worker cancellation, progress,
language appendices, Nearby session protocol and all isolated HAL doubles.

Planning files are contextual evidence rather than production source: the dispatch
brief explicitly requires the entire implementation/tests/documentation diff and
all ledger rulings/deferrals. Original handoff, follow-up plan, acceptance index,
known findings and all630 ledger lines were read in full. Planning patch1–250
was also read; remaining reports are being cross-referenced for their recorded
decisions/evidence, not represented as fully reread source.

Root's focused source trace confirms a Calibre active-WebSocket-upload transition
liveness defect; ordinary Web Back can instead restart, so the finding must name
the managed transition path accurately. Public docs retain more stale claims than
the known converter README, especially old v5/no-language/no-move progress prose.

## Completed assessment — 2026-09-07

**Specification verdict: FAIL pending the six Important findings below and the
explicit final acceptance gates.** The required feature families are present,
but supported browser output, library failure recovery, and lifecycle integration
have concrete gaps. **Quality verdict: CHANGES REQUIRED.** No Critical finding.
Six Important findings and three Minor findings form the single fix wave. This
review does not authorize a commit, push, user-data export or SDK change.

Evidence is the complete current implementation/test/public-documentation diff,
not old task approvals. Root's latest full native checkpoint is 792/792 before
later input/simulator fixes; subsequent focused menu regressions passed. Those
facts are historical evidence, not a fresh final whole-port pass. Current S3
artifacts, final native/converter/simulator runs, and physical acceptance remain
separate gates after fixes. The only reviewer writes are this report.

### Important F1 — active upload can permanently stall a managed transition

**Locations:** `src/activities/ActivityManager.cpp:190`, `:229`;
`src/activities/network/CalibreConnectActivity.h:47`,
`CalibreConnectActivity.cpp:109`, `:172`;
`src/activities/network/CrossPointWebServerActivity.h:89`;
`src/network/CrossPointWebServer.cpp:480`.

Calibre Back queues `finish()` while an incomplete WebSocket upload can still be
active. `prepareToSuspend()` returns false until that upload finishes, and the
default policy retains the pending transition. On the following iteration
ActivityManager suppresses the activity's ordinary loop. That same loop is the
only owner calling `handleClient()`/`wsServer.loop()`, which delivers completion
or disconnect and clears the upload flag. No independent upload timeout clears
it. The activity cannot reach `onExit()` to stop the server, and neither normal
completion nor client disconnect can unblock the transition.

Root independently confirmed the exact pinned WebSockets synchronous callback
path in `/private/tmp/crossink-manga-final-upload-suspension-probe.md`. This is a
source-proven liveness defect; no runtime probe was claimed. Ordinary Web Back
can silently restart when network boot is ready, so this finding does not claim
that every Web Back path stalls. Calibre Back is directly reachable; managed
Home/push transitions share the same dependency. `main.cpp:1449` also suspends
ordinary pumping while a deep-sleep retry is pending, so include that caller when
choosing the quiescence fix.

**Remedy:** keep a bounded main-owner transfer pump runnable while draining, or
explicitly cancel/close an active transfer through a main-owner quiescence API
before accepting navigation/sleep. Do not move callbacks or SD work to an arbitrary
task, and do not resume ordinary input that can replace pending intent. Add an
actual incomplete START/upload → Back/Home/sleep → completion/disconnect or bounded
abort regression; a stationary simulator HTTP listener does not exercise this
hardware-owned WebSocket pump. On C3 and S3, interrupt a disposable upload and
confirm navigation/sleep completes, the file is closed, and partial data follows
the existing abort policy.

### Important F2 — canonical page detection loses sparse and mixed-format pages

**Locations:** `lib/MangaPanel/MangaBook.cpp:328`, `:363`, `:416`;
`test/manga_book/MangaBookTest.cpp:124`; `docs/manga-storage.md:59`.

The fast path selects one extension only when page zero and the final indexed
page both exist with that extension. Otherwise `legacyPage()` assigns dense
positions to the files it enumerates. A valid four-page book with full images
`page_0000.jpg` and `page_0002.jpg` therefore resolves physical page 1 to image 2,
then reports no overview for physical page 2. OCR, panel records and saved position
still use their original indexes, so the wrong full-page art is associated with
them. A complete JPG/PNG/JPG sequence instead chooses JPG and treats the PNG
middle page as absent. The current test intentionally asserts single-extension
priority but does not cover these real producer layouts.

This is an explicit original-handoff requirement, not a proposal for a new
browser converter. Read-only inspection of the linked upstream browser source
confirms it conditionally omits full images except page zero and preserves each
page's JPG/JPEG/PNG extension: [manga-ui.js, lines 668–701](https://github.com/eszter007/matcha-reader-tools/blob/main/js/manga-ui.js#L668).
The [producer README](https://github.com/eszter007/matcha-reader-tools#fidelity-to-the-firmwares-python-tools)
also describes mixed retained pages in its panels-only output. The browser source
was inspected on 2026-09-07; this is current external producer evidence, distinct
from the pinned desktop Python reference. No browser conversion, model download,
OCR call or user-image upload was performed.

**Remedy:** preserve numeric `page_NNNN` identity even with holes or per-page
extensions. Keep the complete uniform-list fast path, use bounded direct canonical
probes or validated sparse canonical discovery for the fallback, and reserve dense
legacy ordering for genuinely noncanonical filenames. Define duplicate-extension
priority explicitly rather than confusing a hole with another page. Update the
old single-extension test/documented rule in light of the binding interoperability
requirement. Add independent sparse/mixed browser-output fixtures, including an
absent final image, only cover plus crops, and a retained middle full page. Assert
exact page paths, absent-overview crop fallback and OCR/page alignment. Verify the
same disposable mixed book on C3 and S3 without changing its portable files.

### Important F3 — Recent Books deletion bypasses transactional cleanup

**Locations:** `src/activities/home/RecentBooksActivity.cpp:209`–`:229`;
`src/activities/home/RecentBooksGridActivity.cpp:534`–`:554`;
`src/activities/home/BookActions.cpp:112`.

Both Recent Books layouts collect a snapshot, call raw `Storage.removeDir()`, and
only clean metadata after total success. Partial physical deletion returns early
with no durable outcome or recovery journal. After successful content removal,
metadata failures are merely logged and recents are still removed; bookmark
cleanup uses the legacy void API. The handlers also never clear matching
`APP_STATE` resume/sleep references and skip ordinary-file entries nested inside
a manga snapshot. They therefore diverge from the shared browser/HTTP/USB
transaction introduced by Task 10c and cannot retry lost bookkeeping after reboot.

**Remedy:** route the selected manga root through `BookFolderMutation::remove()`
and its existing result/owner-reload gate, as FileBrowser does. Preserve normal
non-manga behavior unless the existing shared route already covers it. Surface
RecoveryPending/StorageError consistently and reload the view only after the
appropriate outcome. Add both Recent list/grid action regressions for successful
resume-reference cleanup, a partial delete, and a metadata-stage failure followed
by recovery; retain metadata for surviving/recreated paths. On device, use a
throwaway manga with bookmarks/progress and confirm deletion from both views has
the same cleanup and reboot behavior as deletion from Books.

### Important F4 — library completion action forgets a partially saved edit

**Location:** `src/activities/home/BookActions.cpp:233`–`:250`.

`toggleBookCompleted()` creates new book/global snapshots, applies a relative
increment/decrement, and discards `saveReadingStatsWithRetry()`'s per-target
result when either target remains failed. It retains no dirty edit or frozen
snapshot for a later explicit retry. Example: an unfinished book and global
count 7; both book writes fail but the global save succeeds at 8; the action
returns false. Repeating Mark finished reloads the still-unfinished book and
increments the already-saved global count to 9. Conversely, book success/global
failure followed by Mark unfinished can decrement the count of other books.
Suppressing recents/move side effects at line 250 does not prevent this corruption.

This exceeds the accepted per-file power-loss limitation: the program is still
running and the ordinary action itself discards a known recoverable result.
The approved Task 10b rule requires completion edits to remain dirty until their
required targets publish; `ReadingStatsEditState` handles this for BookStats,
while this library path does not.

**Remedy:** retain a bounded completion-edit transaction in the owning action/UI
flow, with the exact desired completion value, snapshots and successful-target
mask, and offer explicit retry without rereading/toggling the half-published state.
Apply recents/move side effects only when the retained operation completes.
Do not add a cross-file durable journal solely for this fix. Add tests for both
partial-success directions, persistent failure and later retry, proving global
counts and dates change once. On hardware, exercise a disposable book under an
injected save failure and verify both per-book status and global count after retry.

### Important F5 — full-cover fallback starts uncancellable source work after expiry

**Locations:** `src/activities/boot_sleep/SleepActivity.cpp:735`–`:736`;
`src/activities/boot_sleep/SleepCoverAssets.cpp:67`, `:108`, `:267`.

Full sleep-cover preparation receives the whole-attempt cancellation token, but
`cachedCoverPathFor()` is called immediately afterward even when preparation
returned Cancelled. Its manga branch calls `fullMangaPath()`, reopens and validates
the book index, resolves legacy image paths if necessary, and probes the source
codec dimensions with no cancellation token. Even an already-expired budget can
therefore start another complete source-discovery/dimension pass. Those reads do
not update the budget's poll-gap diagnostics. This defeats the accepted
whole-attempt policy; the issue is starting new unbounded work after cancellation,
not claiming that a single SD operation has a hard deadline.

**Remedy:** resolve and retain the usable cache path/geometry during budgeted
preparation, or supply a cache-only fallback that does not reopen the source after
expiry. Propagate the same token through any remaining source discovery/probing
and stop before starting work when already cancelled. Add a real
SleepCoverAssets/SleepActivity test where the budget expires before preparation
and during source discovery, asserting no subsequent source/index pass and a
usable prevalidated cached/default fallback. On C3/S3 record stage, elapsed and
maximum poll gap with a slow/large disposable book; 2500 ms remains cooperative
policy, not a measured guarantee.

### Important F6 — ordinary PDF converter tests crash in native-module cleanup

**Location:** `test/manga_converter/test_converter.py:42`.

The fixture patches the entire `sys.modules` mapping to block two optional model
imports. When the optional PDF test imports PyMuPDF, `patch.dict` cleanup removes
unrelated newly loaded SWIG/native modules. Root reproduced a native cleanup crash
with Python 3.14.7, Pillow 12.3.0 and PyMuPDF 1.28.2; faulthandler points to
`SWIG_Python_DestroyModule` under `_clear_dict` in
`/private/tmp/crossink-manga-pdf-crash.log`. Preimporting fitz makes all ten tests
pass, but masks the fixture lifetime defect. Source review confirms the broad
module-table patch and the conditional real PDF test.

**Remedy:** block optional model imports narrowly and restore only keys owned by
the fixture. Preserve socket, subprocess and model/cloud guards. Do not require
fitz preimport or skip an installed PDF dependency. Run ordinary unittest discovery
with `/private/tmp/crossink-manga-pdf-validation/bin/python`, including two-page
PDF order/metadata, then update the validation documentation. No device test is
needed for this host-only fixture fix; generated PDF output still belongs in the
normal reader-format smoke coverage.

### Minor M1 — published feature documentation describes obsolete stages

**Locations:** `tools/manga_convert/README.md:6`, `:27`, `:41`, `:138`;
`docs/manga-format.md:7`, `:70`, `:120`;
`docs/manga-progress.md:40`–`:59`; `docs/file-formats.md:36`.

The converter README says caches/dictionary integration are still under development,
no dependencies were installed, and PDF was not exercised. The progress document
still says stats_v5 has no language dimension, moves do not migrate state, and
bookmark deletion exposes only a void result. Format docs describe a future
navigation layer and hardware work deferred until reader integration. These
conflict with the implemented v6/v4 statistics, transactions, checked bookmark
API and accepted reader integration.

**Remedy:** update current public docs to the final implemented behavior and fresh
validation evidence, retaining genuine physical/optional-cloud limitations. Keep
historical task reports historical; do not rewrite old results as current passes.
Review the complete user-facing manga docs together, including sparse canonical
rules fixed by F2. Validate document commands/paths against final artifacts.

### Minor M2 — obsolete Nearby protocol mirrors and wrappers remain

**Locations:** `src/activities/network/NearbyStatsSyncActivity.h:50`, `:60`, `:75`;
`NearbyStatsSyncActivity.cpp:285`, `:385`, `:478`.

`peerStatsSaved_`, `localStatsSent_`, `localStatsAcked_`, `syncStartedMs_` and
`lastStatsSendMs_` are written but no longer drive live protocol behavior. The only
read of localStatsSent_ is in the uncalled `sendLocalStats()` wrapper;
`sendDeviceName()`/`sendAck()` are likewise obsolete. Session owns retries,
acknowledgement and completion. Retaining these creates two apparent protocol
owners and obscures the actual tested state machine.

**Remedy:** remove only the dead members, assignments and uncalled wrappers.
Preserve live display/identity fields, `lastHelloMs_`, `sendHello()`, the callbacks
and final-ACK retry behavior. Existing Nearby protocol tests plus both hardware
builds are appropriate; on a pair of devices confirm supported/legacy mismatch
states remain unchanged. This confirms the previously parked Low on its merits.

### Minor M3 — new default ditherer constructor leaves row counter indeterminate

**Location:** `lib/GfxRenderer/BitmapHelpers.h:236`, `:356`.

Changing the constructor to a fallible `begin()` API removed rowCount's previous
constructor initialization. `begin()` sets it after successful allocation, and
all reviewed production callers check success before processing, so no current
rendering failure is demonstrated. Nevertheless a default-constructed object's
`isReverseRow()` can read an indeterminate member and cppcheck correctly identifies
the missing initialization. This is introduced API hygiene, not a baseline warning.

**Remedy:** initialize `rowCount = 0` in the member declaration. Keep begin's
successful reset and the existing fallible allocation checks. Existing real-codec
and dither coverage is sufficient; no new implementation-mirroring test is needed.

## Original handoff — all 50 checkboxes reconciled individually

Row IDs follow the seven sections of
`2026-09-05-manga-port-handoff.md`; the line column identifies each original
checkbox. **Implemented** means source plus appropriate automated evidence exist,
not physical certification. **Gap** names a finding. **Physical pending** is an
explicit incomplete acceptance condition, never a software pass. Test names below
refer to their native directory under `test/`; simulator evidence is indexed in
the final acceptance index and SDD ledger. No checkbox is silently waived.

| ID / handoff line | Individual requirement | Current source and evidence | Disposition |
| --- | --- | --- | --- |
| 1.1 / 38 | Marker recognition regardless of name/depth | `MangaBook.cpp:310`; `ReaderActivity.cpp:226`; MangaBook nested arbitrary-name tests | Implemented; mutation snapshot bounds do not limit ordinary recognition. |
| 1.2 / 39 | IDX/DAT versions, endian, widths, offsets, optional fields | `MangaFormat.cpp:43`, `:53`, `:97`; original-writer literals, unaligned and prefix-truncation manga_format tests | Implemented, v2 only, checked 64-bit extents and 32 KiB records. |
| 1.3 / 41 | Preserve dimensions, rectangles/order, UTF-8 and translation | `MangaFormat.cpp:82`, `:97`, `:120`; original Japanese/NUL fixture and translation cursor tests | Implemented; byte-preserving decoder, no invented coordinates. |
| 1.4 / 43 | Metadata/language/legacy/fallback | `MangaFormat.cpp:137`; `MangaBook.cpp:164`; manga_book large-field/OOM/legacy tests | Implemented; optional allocation failure falls back to folder title. |
| 1.5 / 45 | Optional TOC labels/indexes | `MangaFormat.cpp:171`; `MangaBook.cpp:196`, `:230`; manga_format and sequential/backward TOC tests | Implemented; unreachable entries handled by selection rather than rewriting portable data. |
| 1.6 / 46 | Canonical and legacy discovery/order | `MangaBook.cpp:328`, `:363`; canonical/priority/tied-legacy tests | Gap F2 for sparse/mixed canonical families; genuine legacy natural/pinned ordering retained. |
| 1.7 / 48 | Panels subdirectory and flat crops | `MangaBook.cpp:428`; manga_book subdirectory/regular-panels-file/mixed-crop tests | Implemented; per-crop BMP/JPG priority, selected directory does not mix stale root crops. |
| 1.8 / 49 | Panel-only/zero-panel/missing/malformed bounded behavior | `MangaBook.cpp:86`, `:147`; `MangaNavigation.cpp`; manga_book/navigation malformed and OOM tests | Parsing/bounds implemented; sparse overview identity still Gap F2. |
| 1.9 / 51 | Document contract and original converter fixtures | `docs/manga-format.md`; original writer hash and five binary fixtures; manga_format independent literals | Implemented binary contract; current-stage prose Gap M1. |
| 2.1 / 57 | Pinned converter and dependency/setup docs | `tools/manga_convert/convert_manga.py:1`; retained MIT license; converter source diff | Converter port implemented; docs Gap M1. |
| 2.2 / 58 | Folder/CBZ/ZIP/EPUB/PDF and actual dependencies | `convert_manga.py:237`, `:398`; converter archive/spine/real optional PDF tests | Code paths implemented; ordinary complete test gate blocked by F6. |
| 2.3 / 60 | Ordering/explicit order/metadata/overrides/chapters | `convert_manga.py:237`, `:922`, `:981`, `:1279`; converter order/archive/EPUB/TOC tests | Implemented with documented pinned ordering/regex limitations. |
| 2.4 / 62 | Detection/Japanese order/YOLO/gutter/margins/naming | `convert_manga.py:533`, `:700`, `:719`, `:1279`; real gutter/crop and mocked OCR boundary tests | Implemented; YOLO model/live inference deliberately unexecuted, optional imports guarded. |
| 2.5 / 64 | X3/X4, mono/dither, preprocessing, limit, no OCR | `convert_manga.py:1248`, `:1279`; mono/sizing/progressive-JPEG converter tests | Implemented; inherited unresized mono alpha behavior explicitly documented. |
| 2.6 / 66 | OCR/translation output and transformed boxes | `convert_manga.py:874`, `:1279`; independent crop-origin/xywh expected bytes | Implemented approved OCR defect correction; old bytes remain readable. |
| 2.7 / 68 | Audit rewrite/checkpoint/resume honestly | `convert_manga.py:906`; README Interruptions and reruns | Implemented audit: whole-index then data rewrite, no checkpoint loader, fresh output advised. No resumability claim. |
| 2.8 / 70 | Optional OCR/no unintended paid calls | `convert_manga.py:1279`; converter socket/subprocess/model guards | Implemented runtime options and offline tests; repair fixture lifetime F6 without weakening guards. |
| 2.9 / 73 | Existing browser-tool output loads; no new browser converter | `MangaBook.cpp:328`, `:416`; upstream browser manga-ui.js:668–701 source inspected | Gap F2; ordinary v2 bytes match but sparse/mixed page families do not. |
| 3.1 / 78 | Overview and ordered forward/reverse panel navigation | `MangaReaderActivity.cpp:196`, `:202`; `MangaNavigation.cpp`; manga_navigation/menu tests | Implemented; image identity failure F2 can display wrong overview. |
| 3.2 / 80 | Page-turn/Confirm/Back/held Back semantics | `MangaReaderActivity.cpp:824`, `:662`; actual button/prefetch one-shot regressions | Implemented: panel Confirm lookup, overview menu, held Back browsing. |
| 3.3 / 83 | Per-book panel/rotation settings and overlay geometry | `MangaProgressStore.cpp`; `MangaReaderActivity.cpp:326`, `:1335`; manga_geometry, progress, status, OCR geometry tests | Implemented; legacy crop overlay policy explicitly uses full page/text fallback. Physical orientation check pending for final image. |
| 3.4 / 86 | Automatic panel mode; zero-panel overview fallback | `MangaNavigation.cpp`; `MangaReaderActivity.cpp:149`; panel-only/zero-crop tests | Implemented transition rules; Gap F2 for sparse producer image presence. |
| 3.5 / 88 | Chapters/percent/bookmarks/position/reopen/sleep | `MangaReaderActivity.cpp:178`, `:286`, `:306`, `:1364`; selectors/progress/menu/bookmark tests and sleep smokes | Implemented software; upload sleep integration F1; final physical sleep/wake pending. |
| 3.6 / 90 | Metadata/status/progress/translation and real menu inventory | `MangaReaderActivity.cpp:257`, `:326`, `:576`, `:772`; 16-row menu, real status pixels and translation tests | Implemented; full mono/gray button/touch menu execution accepted after scoped round 3. |
| 3.7 / 92 | ActivityManager/RenderLock/logical input/theme/i18n/touch | `ActivityManager.cpp:409`; `MangaReaderActivity.cpp:824`, `:1284`; mapped-input and actual touch smokes | Manga path implemented; shared integration Gap F1. No SDK mutation. |
| 3.8 / 95 | Fast navigation and explicit physical cleanup policy | `MangaReaderActivity.cpp:1219`, `:1284`; shared lookup refresh flow; status/gray plane tests | Implemented policy; final hardware ghosting/cleanup acceptance pending. Task 7 user observation is partial historical evidence only. |
| 4.1 / 100 | Mono/gray decode, aspect, rotation, crop cache | `MangaReaderActivity.cpp:1078`, `:1219`; `MangaBitmapPixels.cpp`; manga_geometry/bitmap/pixel_cache tests | Implemented; final physical shades/rotation pending. |
| 4.2 / 102 | Cache and fresh geometry/pixels match | `ImageToFramebufferDecoder`/JPEG/PNG cache-only paths; `MangaReaderActivity.cpp:1061`; 64 real-codec comparisons/pre-change goldens | Implemented automated evidence; physical display equivalence pending. |
| 4.3 / 103 | Cache geometry without source dimension decode; canonical fast path | `MangaPixelCache::sourceIdentity`; `MangaReaderActivity.cpp:1078`; identity/dimension tests | Implemented cache hit path; content CRC reads source intentionally. Canonical fallback Gap F2. |
| 4.4 / 105 | Identity/version/settings/corruption distinct from book format | `MangaPixelCache.cpp:100`; `docs/manga-pixel-cache.md`; identity mismatch, truncation, CRC and rename tests | Implemented MPX1 envelope; raw EPUB cache payload unchanged. |
| 4.5 / 108 | Disposable caches and safe fallback | `MangaReaderActivity.cpp:1078`; `MangaPixelCache.cpp`; malformed cache/failed second rename/OOM tests | Implemented; cache warmth can be lost but invalid pixels are not exposed. |
| 4.6 / 109 | One framebuffer, bounded C3 allocations, fallibility/capabilities | `MangaReaderActivity.cpp:76`; 9216-byte owned BMP scratch, cache rows, fallible codec owners; font-pool and fault tests | Implemented architecture; actual C3/S3 internal heap/largest block/PSRAM and QR stack watermark pending. M3 initializes idle dither state. |
| 5.1 / 114 | Idle useful next page/first-next panel | `MangaReaderActivity.cpp:1391`; `MangaPrefetch.cpp:100`; worker/real lifecycle smokes | Implemented one-slot policy after idle delay, current panel then next page where useful. |
| 5.2 / 115 | Shared foreground/warming geometry | `MangaImageGeometry.cpp`; `MangaPrefetch.cpp:51`; geometry/worker byte comparisons | Implemented shared fitting/orientation/BMP no-upscale policy. |
| 5.3 / 116 | Worker never changes renderer/stack | `MangaPrefetch.cpp:175`; renderer-free decoder APIs; worker renderer-access counter test | Implemented; tests assert zero renderer access with real JPEG/PNG. |
| 5.4 / 117 | Bounded jobs, cancel/exit/shutdown, responsive foreground | `MangaPrefetch.cpp:100`, `:114`, `:140`; `MangaReaderActivity.cpp:1356`; state and boundary intent tests | Implemented worker contract; broader shared upload quiescence Gap F1. Physical cancellation latency pending. |
| 5.5 / 119 | Temp publication and real SD coordination | `MangaPrefetch.cpp:175`; `MangaPixelCache.cpp`; `MangaReaderActivity.cpp:1356`; final-sync cancellation and held-file stress | Implemented worker/foreground ownership; F1 transfer lifecycle integration remains. Hardware SD concurrency acceptance pending. |
| 5.6 / 121 | No corruption/leaks/heap degradation through rapid/sleep flows | `MangaPrefetch.cpp:140`; worker close/count/generation tests; deterministic grayscale stress and repeated activity smokes | Automated contract evidence present; final physical long-run heap/tasks/handles/latency pending. |
| 6.1 / 126 | Browser/Home/Recent/Continue integration with existing formats | `FileBrowserActivity.cpp:815`; `ReaderActivity.cpp:226`; `HomeActivity.cpp:47`; recent views; real Books/EPUB smokes | Implemented; original dictionary/EPUB/TXT/XTC/Anki routes retained. |
| 6.2 / 128 | Japanese metadata, cover, progress and book actions | `BookActions.cpp:220`; Home/Recent adapters; actual title/progress/actions smoke | Implemented displays; action recovery Gaps F3/F4. Final SD-font cycling pending. |
| 6.3 / 129 | Size-aware 1-bit thumbnail, first page never crop | `MangaCover.cpp:319`; `MangaBook::pageImagePath`; 50 real-codec cover goldens at UI sizes | Implemented strict MCG3 geometry/digest and page-zero source; final on-device cover matrix pending. |
| 6.4 / 131 | Cover cancellation/OOM/missing/damage | `MangaCover.cpp:319`; Home/Recent cover epochs; sleep token; all cancellation/allocation/rollback cover tests | Main conversion implemented; full sleep fallback Gap F5. Physical timing unmeasured. |
| 6.5 / 132 | App stores, progress/bookmarks/settings/time/language/completion | `MangaProgressStore`; `MangaStatsCommit.cpp:11`; Book v6/Global v4, language and save tests | Implemented bounded local-day/summary design; library completion Gap F4. |
| 6.6 / 135 | Moves/rename/delete/cache-clean preserve correct durable state | `BookFolderMutation.cpp:1073`, `:1107`; `BookCacheUtils.cpp:342`; transaction fault sweep/live routes | Moves/shared deletion/cache preservation implemented; Recent Books deletion Gap F3. Raw external rename remains documented identity limitation. |
| 7.1 / 140 | Ordered OCR text/coordinates exposed | `MangaPageTextSource.cpp:52`, `:112`, `:151`; OCR source tests | Implemented full-page block transforms, exact order and bounded UTF-8 storage. |
| 7.2 / 141 | Reuse Japanese/StarDict names/grammar/segmentation/deinflection | `MangaReaderActivity.cpp:662`; `EpubReaderWordLookupActivity.cpp:248`; inherited engine and expanded Japanese tests | Implemented shared engine, not a manga dictionary fork. |
| 7.3 / 143 | Progressive discovery/paging/cancel/empty/errors/font restore | `EpubReaderWordLookupActivity.cpp:275`, `:435`, `:468`; actual OCR/empty/unavailable/forced-exit smokes | Implemented; full real dictionary latency and repeated SD-font cycles pending. |
| 7.4 / 145 | Scan cache keyed to scope/text/full dictionary identity | `MangaPageTextSource.cpp:296`; `EpubReaderWordLookupActivity.cpp:2082`, `:2118`, `:2142`; full-index replacement tests | Implemented; immutable active-source contract, fresh activation verification. Large indexes may bypass useful warm loading without delaying first definition. |
| 7.5 / 147 | History/saved text/clippings/nested lookup and separate upstream audit | `MangaReaderActivity.cpp:784`; `MangaPageTextSource.cpp:263`; shared lookup history/clipping paths; OCR audit and exact clipping/nested smokes | Implemented approved full-page/text fallback and shared floating UI; final physical cycles pending. |
| 7.6 / 150 | Reading requires no live OCR/cloud translation | `MangaReaderActivity.cpp:662`, `:772`; stored page text and translation readers; offline fixtures | Implemented entirely on-device from portable bytes; no network dependency in reader path. |

## Binding follow-up plan and ledger reconciliation

The complete `progress.md` ledger was read initially through line 630 and again
through its final review/probe additions (651 lines at final read). The table below accounts
for every explicit Ruling, refinement, parked/deferred issue, and preflight row.
Line numbers identify the current ledger; historical build/timing entries remain
historical, including logs whose filenames say green but whose recorded exit was
failure. The later accepted correction supersedes those failed checkpoints.

| Ledger item | Implementation/evidence reconciliation | Disposition |
| --- | --- | --- |
| Preflight row: 10a ↔ 10e lifecycle/cache deletion | `BackgroundSuspension.h:3`; `MangaReaderActivity.cpp:497`, `:1364`; actual drain/menu tests | Implemented for cover/prefetch; shared upload owner gap F1. |
| Preflight row: 10b → 10c versions/day appendices | v6/v4 whole files recognized by `BookMutationStorage`; opaque durable migration tests | Implemented; no day arrays on stack. |
| Preflight row: 10b ↔ 10e timer | `MangaStatsCommit.cpp:11`; automatic/manual `applyMoveLocked`; threshold/tail tests | Implemented; menu/child/lock excluded. |
| Preflight row: 10c → 10e durable cache files | `BookCacheUtils.cpp:342`; `BookMutationStorage::durableCacheName`; cache-deletion tests | Implemented dictionary route/history preservation. |
| Preflight row: 9b/9c → 10e lookup/QR | `MangaReaderActivity.cpp:445`, `:662`, `:824`; one-shot lookup/QR tests | Implemented; direct panel Confirm preserved. |
| Preflight row: 10a ↔ final 10d deadline | `SleepCoverBudget.h`; recorded diagnostics | Policy remains unmeasured; F5 must close fallback bypass. |
| Preflight row: 10a CRC/codec/allocation/epoch | `MangaCover.cpp`; `MangaCoverWork`; real cancellation/allocation tests | Implemented within conversion; F5 at sleep integration. |
| Preflight row: 10b eight buckets/local 730 days/Nearby summary | `ReadingLanguageStats`; `NearbyStatsProtocol`; max appendix/protocol tests | Implemented, remote daily detail explicitly unavailable. |
| Preflight row: 10c existing moves/bounded recovery | `BookFolderMutation`; WebDAV/USB routes; collisions/fault sweep | Implemented route scope; Recent deletion F3 remains integration gap. |
| Preflight row: 10e inventory/auto/status plane rules | 16-action MenuState, `automaticNext`, `MangaStatus`; real status pixels/menu smokes | Implemented; BW text/white patch, gray masks zero, restored BW. |
| Preflight row: 10d final gate after all tasks | Whole review package and frozen source; root final scripts prepared | Review complete; fix/fresh all-board/physical gates pending. |
| Ruling 25: 2500 ms whole-sleep-cover budget, half watchdog | `SleepCoverBudget`; single begin in SleepActivity; poll-gap diagnostics | Implemented policy, Gap F5 after expiry; hardware latency still pending. |
| Ruling 30: full menu rows not waived by globals | `MangaMenuState.h`; `MangaReaderActivity.cpp:257`, `:326` | Implemented all 16 commands and panel menu escape. |
| Ruling 46: accept valid contained cover geometry | `MangaCover.cpp` source-reported dimensions/fit validation; 80×120→200×300 regression | Implemented without changing pre-change pixels. |
| Ruling 58: byte-7 Nearby capability/legacy ACK before mismatch | `NearbyStatsProtocol.cpp:9`, `:96`; two-peer legacy v1/v2/v3 and ACK tests | Implemented; physical updated/legacy pairing pending. |
| Parked 67–77: converter native cleanup + truthful README | Broad module patch and root crash independently reconciled | F6 + M1; no preimport workaround accepted as fix. |
| Ruling 79: backup/local-vs-synced/MAC/cleanup/save contracts | Book/Global load provenance, `ReadingLanguageStats.cpp:379`, `:429`; full backups/MAC-failure tests | Implemented stores; library edit retention missing F4. |
| Ruling 89: pre-lock atomics, no universal lock-free assertion | `BackgroundSuspension.h`; MangaCoverWork | Implemented; C3 SDK masked-atomic investigation is historical, not final choice. |
| Refinement 99: single main writer relaxed-load/release-store epoch | MangaCoverWork main-writer contract and acquire reader; assembly evidence indexed | Implemented; no fetch_add/custom mutex/volatile substitution. |
| Ruling 123: include actual result diagnostics | `SleepActivity.cpp:1155`; ThumbnailDiagnostics | Implemented hit/publication/cancel/failure fields; F5 poll coverage gap remains. |
| Ruling 136: MCG3 40-byte exact geometry/BMP CRC/crossed recovery | `MangaCover.cpp`; file-formats layout; real cross-pair failure matrix | Implemented; v2 disposable invalidation explicit; CRC not authentication. |
| Ruling 158: one retry only failed exact snapshot; dirty completion edits | `ReadingStatsSave.cpp`; BookStats edit state and MangaStatsCommit | Reader/edit stores implemented; library completion F4 violates retention. |
| Parked 214, 268, 276, 294: Nearby Low | Real references searched; Session owns behavior | M2 confirmed; remove only unused state/wrappers. |
| Ruling 215: default retry vs BookStats failure-cancel; no autosleep write loop | `Activity.h:46`; BookStats guard; failure/retry actual sleep/Home smokes | Implemented BookStats; F1 is a new incompatible upload wait owner. |
| Ruling 227: preserve recovery siblings within 16 files/checked HAL | `BookMutationStorage`; snapshot/journal tests | Implemented complete durable families, supported bounds. |
| Ruling 233: clear Quick Lock intent only on cancelled sleep | `main.cpp:909`; actual cancelled then ordinary sleep assertions | Implemented; drain retry retains intent. |
| Ruling 242: positive clean EOF, sticky errors, conservative full directory | HAL checked API; SdFat exact implementation; checked-EOF tests | Implemented for destructive scans; ordinary nonmutating discovery retains documented limitations. |
| Ruling 257: pinned simulator consumer patch with pre/post hashes | scripts/patch_simulator_storage.py and companion manifest/patch; packaging tests | Implemented dependency-local hook; no SDK or published simulator mutation. |
| Ruling 283: frozen originals + 64-byte absent outcome, never redelete | `BookMutationJournal`; `BookFolderMutation.cpp:916`; interruption/recreation/fault tests | Implemented shared transaction; bypass in F3. |
| Ruling 305: QR safe capacities/11552 subtotal/no speculative stack increase | `QrCodePolicy`; actual pinned QR encoder boundary tests | Implemented 78/271/858/1732/2953 and 3917-byte grid. C3/S3 render-stack watermark pending. |
| Ruling 311: idempotent pre-delete stats bank/retry/tail | `MangaStatsCommit.cpp:11`; real daily appendix partial-target tests | Implemented bounded member transaction, no durable queue. |
| Preflight ruling 316–322: validate frozen shared originals before PREPARED/DELETE_STARTED; ordinal/order | `BookFolderMutation` prepareShared/verifyOriginals; strict journal parser/owner JSON tests | Implemented; outputs are not invented before partial deletion. |
| Refined stats ruling 324–333: frozen + live tail, max two transactions, thresholds, cancelled exit | `MangaStatsCommit.cpp`; `MangaReaderActivity.cpp:1034`, `:1364`; full asymmetric failure tests | Implemented exact spans, carry, page/completion mutation ordering and one session. |
| Ruling 339: exact stored FAT spelling and destination ancestors | checked component enumeration; alias/collision tests | Implemented conservative refusal before identity derivation. |
| Ruling 349: terminal finalize until bounded owner reload; inherited string/vector limit | `BookFolderMutation.cpp:1037`; JSON allocator and terminal failure tests | Implemented. Inherited owner string/vector population is not fully fallible; limitation retained honestly. |
| Ruling 367: explicit boot retry recovery screen/exclusive loop/restart | `FullScreenMessageActivity.cpp:24`; `main.cpp:1175`; recovery tests/routes | Implemented explicit retry; no automatic destructive replay or normal input bypass. |
| Task 10c final review findings: complete cache deletion/stage ordinal/USB depth eight | `BookFolderMutation.cpp:758`, `:857`; boundary/failure tests; accepted scoped fix | Addressed in shared engine; F3 is separate missed caller integration. |
| Task 10c live routes: simulator thread vs hardware main owner | Stationary localhost MOVE/collision/delete evidence | Accepted limited evidence; explicitly does not disprove F1. |
| Ruling 492: checked BookmarkStore save gates cache deletion/suspend | checked API; `MangaReaderActivity.cpp:1034`; write/sync/close/remove failures | Implemented; shared legacy callers remain compatible. |
| Deferred 542: outside-menu compiler warnings | HEAD comparison proves unchanged quick-lock rectangle and SettingsAction switch | Baseline triage below; no unsolicited source rewrite. |
| Task 10e R1/R2: auto pending-render liveness and one-shot lookup drain | Current reader loop/pending intent; deterministic deferred/running/finished regressions | Addressed; no repeated user input used to mask lost intent. |
| Task 10e R3: validation matrices/maximum mixed UTF-8 QR | real encoder+draw+stats tests, current touch/menu runs | Addressed software execution after round 3; physical QR margin still pending. |
| Task 10e R5/R6: same-frame edge-before-render completion and harness error gate | `MangaReaderActivity.cpp:824`; MangaSmokeValidationTest; release/active-event runs | Addressed; common error marker validation preserved. |
| Ruling 598: final S3 only after whole-port fixes | C3/capability simulator timing fixes accepted; old S3 tagged pre-fix | Pending root final gate; old S3 outputs not current-source acceptance. |
| Task 10e round 3: simulator tap/swipe mutual exclusion | SIM-only MappedInputManager diff and boundary/full touch tests | Addressed after failed touch runs; no real-driver input change. |
| Final 10d entries: full untruncated review/static triage/source freeze/upload probe | This report, chunk coverage, static table, root source-only probe | Completed review; all findings held for one combined dispatch. |
| Dictionary export/restore and physical acceptance parked throughout | Original 272117145-byte backup preserved; absent device endpoint; user fonts untouched | Still pending explicit export approval/device availability; no bypass or false completion. |

Earlier decisions outside this final ledger were also reconciled: the completion
plan's font pooling is proven by compiled default/noemoji byte equivalence and
pinned compiler include order; no font was removed. Its legacy OCR geometry ruling
is implemented by full-page block highlights or text-area fallback, restoring the
original panel on return. Reader-plan missing crops are skipped, rotation settings
are per book, and initial BW-only staging was superseded by Task 7. Pixel-review
rulings remain deliberate: disposable cache pairs may lose warmth on failure;
BW is replayed into the single framebuffer; high-color BMPs use fixed four-level
quantization while native mono/gray levels are preserved. These are accepted
tradeoffs, not open parity findings. F2 corrects an interoperability gap that the
previous first/last canonical rule failed to account for.

## Static-check triage and non-findings

Root ran cppcheck 2.11 through PlatformIO on 159 changed/new implementation C/C++
paths, default profile, package reporting skipped, all severities configured to
fail. It completed normally in 12.623 seconds with exit 1: 0 high, 14 medium,
46 low reports, deduplicated to 51 diagnostics. This is a valid tool run with
findings, not an execution failure or a warning-free pass. The log is
`/private/tmp/crossink-manga-final-static.log`; every unique diagnostic is accounted
for in the table below. M3 is the actionable introduced initialization issue.

The `void*` arithmetic messages are analyzer type-inference errors: the owners are
`unique_ptr<uint8_t[]>` or `unique_ptr<char[]>`, and production `.get()` has the
corresponding typed pointer. Repeated cooperative cancellation checks intentionally
surround calls that can advance time or change the callback result; treating the
second call as pure would remove required cancellation boundaries. The two
post-backend cancellation guards are defensive checks around a call, with no
functional fault demonstrated. Style suggestions do not justify changing bounded
embedded loops merely to satisfy a preferred STL spelling. No global suppression
or blind rewrite is recommended.

The two parked compiler-warning groups are **baseline**, confirmed by comparison
with HEAD: the quick-lock `Rect` narrowing expression in `main.cpp:524` and the
`SettingsActivity.cpp:1006` action switch were already present unchanged. The
latter's ScreenMargin/ControlsHomeButton handling exists earlier in the settings
routing. Retain the warning evidence; do not label the build warning-free or add
unrelated cleanup to this fix wave.

Other investigated candidates were not promoted:

- Ordinary TXT/XTC deletion intentionally retains the established cache policy;
  the transaction tests assert it and the previous BookActions paths agree.
- Nonmutating legacy image enumeration still has the documented HAL limitation
  around ambiguous low-level EOF. Destructive mutation scans use the new checked
  API. No new destructive failure was established in the ordinary reader path;
  this is distinct from F2's deterministic numeric-page identity defect.
- Large optional title/author/language/TOC fields are bounded by encoded uint16
  lengths and allocated fallibly after the required page buffer. Their worst-case
  payload is approximately 295 KiB and may not fit a full C3 reader; optional
  fallback is intentional, not a claim that every pathological book fits.
- Pixel publication may discard an old disposable pair on rename failure; strict
  identity/payload validation and regeneration are the accepted recovery contract.
- The converter's non-atomic index/data rewrite, no resume loader, stale-output
  rerun behavior, text/count truncation and possible oversized page record are
  explicitly inherited producer limits. OCR endpoint-to-size correction is the
  approved exception. No unrelated pipeline redesign is requested.
- Real-codec native goldens prove deterministic algorithm behavior on these
  fixtures, not e-ink waveforms, SD timing, PSRAM/DMA behavior or hardware heap.
- Local daily statistics intentionally use active-duration calendar compression,
  bounded eight buckets and a 730-row stream. Nearby sends only summaries; no
  remote daily detail or cross-file atomicity is claimed.

## Final acceptance still required after the combined fix

1. Scoped review of the complete F1–F6/M1–M3 delta, including every new regression
   and documentation correction. Keep the original source snapshot as baseline;
   do not stage/commit/push or change SDK integration.
2. Root's fresh sequential native suite, ordinary offline converter discovery with
   installed PDF support, default/Sticky/X4 Pro and both simulator builds. Run the
   final normal manga, OCR, grayscale/prefetch stress, complete menu/failure-retry
   and EPUB regressions; add the newly identified transfer, library-action and
   sparse/mixed producer cases. Record commands, exit codes and artifact hashes.
   Real codec/QR test targets are dependency-conditional, so confirm they were
   actually built/registered rather than trusting a smaller passing count.
3. Physical final-image acceptance on C3 and available S3: exact uploaded artifact,
   nested/canonical/legacy/sparse/panel-only books, labels/fonts, all orientations,
   cold/warm shades, cleanup/ghosting, rapid turns and cancel/sleep/resume,
   disposable transport/delete/failed-save recovery, and updated/legacy Nearby.
   Clear only disposable manga cache for cold comparisons. Measure internal free
   heap and largest block, worker/render stack high-water marks, open handles/tasks,
   and S3 PSRAM. The QR source stack subtotal is 11552 of 16384 bytes before the
   complete call chain; abundant PSRAM does not prove that margin or internal
   allocation safety. Record cover stage/elapsed/max poll gap separately from
   the 2500 ms policy.
4. Large real Japanese and StarDict indexes: first-definition latency separately
   from complete identity verification and cache usefulness, repeated nested
   lookup/back/font restoration, and source replacement between activations.
5. User dictionary restoration remains separate: backup
   `/dictionaries/jp.user-backup-20260905` is untouched; active `/dictionaries/jp`
   is the tiny fixture. The prior automatic approval rejection of the
   272117145-byte local export remains pending explicit approval. Do not infer
   export approval from continuation or connection messages. Preserve the backup,
   NotoSansJP and BookerlyJP. No reviewer export/device mutation occurred.

These physical and restoration items are not new software findings and cannot be
closed by native/simulator passes. Existing user confirmation applies to Task 7,
not the later unflashed prefetch/OCR/library/menu implementation.

## Final coverage and evidence integrity

All 170 implementation text entries, 115 test text entries and nine public
documentation entries were read in their entirety relative to HEAD, including
untracked files. Thirteen binary fixtures received separate hash, format/content
and provenance checks. The baseline `.codegraph/.gitignore` five-line addition was
read and preserved as unrelated local-index policy. Planning's 55 entries are
listed below as contextual artifacts, with authoritative requirements, every SDD
ruling/deferral, relevant final evidence and earlier explicit design decisions
read directly. Planning output is not misrepresented as 8234 fully reread source
lines: the dispatch requires its use as context, while explicitly requiring full
implementation/tests/public-documentation reads.

Patch coverage is exhaustive: implementation 1–24706, tests 1–9603,
documentation 1–860, baseline-index 1–9. The persistent checkpoints above record
all chunk boundaries and repaired truncations. A later broad planning search was
truncated and used only for navigation; direct targeted reads of the requirements,
ledger and selected decisions supply the actual evidence. No truncated search is
counted as complete source coverage.

Focused reviewer verification on 2026-09-07:

- SHA256 of every one of the 307 implementation/test/public-documentation manifest
  files still matched the frozen package. No source drift occurred during review.
- All 13 binary fixture hashes matched the manifest. Pillow decoded all eight
  image fixtures: 129×193 RGB JPEG/PNG, and 80×120 baseline/progressive JPEG,
  grayscale/palette/RGBA PNG and top-down 1-bit BMP. Their decoded mode, dimensions
  and deterministic corner pixels matched the documented synthetic provenance.
- Original format fixture bytes were independently inspected: v2 three-page index
  `(0,87,800,480)`, `(87,2,65535,0)`, `(89,0,0,65535)`; two ordered page-zero panels,
  Japanese text with embedded NUL, translation and an empty page; metadata legacy
  and `ja` trailer; TOC Cover/第一章/第二章 with indexes 0/1/2. Original pinned desktop
  writer SHA256 is `ed62096b4fe77f7c10ef57985da685c366c07e8fa88304140f9e56d6acbb0293`,
  confirmed directly with read-only `git show` against the reference repository.
- `git diff --check` exited 0; HEAD remains
  `ea03940023c1705fed80ea64cc0de3208f38321a`. SDK remains
  `1e8ee543edca397f2b8747811f5a88f1bc35d233`; assets/tabler-icons remains uninitialized.
  Git status contains the pre-existing uncommitted port and this report, with no
  staged or committed changes by the reviewer.
- No PIO, broad passing suite, serial, cloud OCR/model invocation, user-data export,
  staging, commit, push, reference edit or SDK mutation was performed by this
  reviewer. The read-only public browser-source inspection was necessary to
  reconcile handoff checkbox 2.9 and exposed F2.

### All 51 unique static diagnostics

| Location / diagnostic | Classification and action |
| --- | --- |
| `lib/Dict/DictIndex.cpp:760` — `constParameterReference` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `lib/Dict/DictIndex.cpp:227` — `useStlAlgorithm` | Optional style suggestion; explicit bounded/early-exit loops are clear, no correctness finding. |
| `lib/Dict/DictIndex.cpp:605` — `useStlAlgorithm` | Optional style suggestion; explicit bounded/early-exit loops are clear, no correctness finding. |
| `lib/Dict/DictIndex.cpp:212` — `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |
| `lib/Dict/DictIndex.cpp:375` — `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |
| `lib/Dict/DictIndex.cpp:703` — `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |
| `lib/Dict/DictIndex.cpp:706` — `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |
| `lib/GfxRenderer/BitmapHelpers.h:236` — `uninitMemberVar` | Introduced valid initialization warning; M3, initialize rowCount. |
| `lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp:538` — `knownConditionTrueFalse` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/Epub/Epub/converters/DirectPixelWriter.h:36` — `constParameterReference` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `lib/Epub/Epub/converters/PngToFramebufferConverter.cpp:474` — `knownConditionTrueFalse` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/Epub/Epub/converters/PngToFramebufferConverter.cpp:168` — `constVariablePointer` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `lib/Epub/Epub/converters/PngToFramebufferConverter.cpp:176` — `constVariablePointer` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `lib/JpegToBmpConverter/JpegToBmpConverter.cpp:116` — `constVariable` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `lib/JpegToBmpConverter/JpegToBmpConverter.cpp:154` — `constVariable` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `lib/MangaPanel/MangaBook.cpp:83` — `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |
| `lib/MangaPanel/MangaCover.cpp:172` — `identicalConditionAfterEarlyExit` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/MangaPanel/MangaCover.cpp:348` — `identicalConditionAfterEarlyExit` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/MangaPanel/MangaCover.cpp:383` — `identicalConditionAfterEarlyExit` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/MangaPanel/MangaCover.cpp:399` — `identicalConditionAfterEarlyExit` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/MangaPanel/MangaCover.cpp:259` — `knownConditionTrueFalse` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/MangaPanel/MangaCover.cpp:356` — `knownConditionTrueFalse` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/MangaPanel/MangaCover.cpp:434` — `knownConditionTrueFalse` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/MangaPanel/MangaPixelCache.cpp:169` — `identicalConditionAfterEarlyExit` | Intentional repeated callback/deadline poll across work; retain cancellation boundary. |
| `lib/PngToBmpConverter/PngToBmpConverter.cpp:150` — `constVariable` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `lib/PngToBmpConverter/PngToBmpConverter.cpp:179` — `constVariable` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `lib/hal/HalStorage.cpp:118` — `useInitializationList` | Existing internal HAL construction style; no introduced functional issue or required rewrite. |
| `lib/hal/HalStorage.cpp:233` — `noExplicitConstructor` | Existing internal HAL construction style; no introduced functional issue or required rewrite. |
| `lib/hal/HalStorage.cpp:24` — `unusedStructMember` | Default-profile-only view; massStorage is used by capability-gated S3 USB paths, preserve. |
| `src/activities/reader/MangaPageTextSource.cpp:151` — `passedByValue` | Small borrowed PageView value, bounded; optional const-reference style, no demonstrated performance defect. |
| `src/activities/reader/MangaPageTextSource.cpp:263` — `passedByValue` | Small borrowed PageView value, bounded; optional const-reference style, no demonstrated performance defect. |
| `src/activities/reader/MangaProgressStore.cpp:82` — `useInitializationList` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `src/activities/reader/MangaQrPayload.cpp:75` — `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |
| `src/activities/reader/ReadingLanguageStats.cpp:275` — `useStlAlgorithm` | Optional style suggestion; explicit bounded/early-exit loops are clear, no correctness finding. |
| `src/util/BookDeletionSnapshot.cpp:78` — `shadowFunction` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `src/util/BookDeletionSnapshot.cpp:103` — `shadowVariable` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `src/util/BookFolderMutation.cpp:619` — `shadowFunction` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `src/util/BookFolderMutation.cpp:627` — `shadowFunction` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `src/util/BookFolderMutation.cpp:640` — `shadowFunction` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `src/util/BookFolderMutation.cpp:710` — `shadowFunction` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `src/util/BookFolderMutation.cpp:361` — `clarifyCalculation` | Precedence matches intended bit-test ternary; optional parentheses, not a correctness defect. |
| `src/util/BookFolderMutation.cpp:277` — `useStlAlgorithm` | Optional style suggestion; explicit bounded/early-exit loops are clear, no correctness finding. |
| `src/util/BookFolderMutation.cpp:298` — `useStlAlgorithm` | Optional style suggestion; explicit bounded/early-exit loops are clear, no correctness finding. |
| `src/util/BookFolderMutation.cpp:864` — `useStlAlgorithm` | Optional style suggestion; explicit bounded/early-exit loops are clear, no correctness finding. |
| `src/util/BookMutationJson.cpp:84` — `redundantCondition` | Redundant EOF/control-byte rejection is harmless; optional simplification only. |
| `src/util/BookMutationJson.cpp:89` — `shadowFunction` | Optional const/initializer/naming style; reviewed source has no corresponding functional defect. |
| `src/util/BookMutationStorage.cpp:204` — `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |
| `src/util/BookMutationStorage.cpp:211` — `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |
| `src/util/JapaneseDictionaryBackend.h:28` — `knownConditionTrueFalse` | Defensive cancellation recheck after backend call; no functional defect established, retain unless proven unnecessary. |
| `src/util/DictionaryEngine.cpp:390` — `knownConditionTrueFalse` | Defensive cancellation recheck after backend call; no functional defect established, retain unless proven unnecessary. |
| `src/util/StarDictBackend.cpp:393` — `arithOperationsOnVoidPointer` | Type-inference false positive: typed unique_ptr array owner, no void-pointer arithmetic in C++ source. |

### Binary fixtures — individual provenance coverage

| Fixture | Bytes / SHA256 | Inspection |
| --- | --- | --- |
| `test/image_cache_decode/fixtures/pattern.jpg` | 20645 / `0fe54148fe694e0766965cf80d10aeafa1c75481ecac21a163665622a85c2349` | Synthetic pattern provenance and codec goldens read; decoded mode/dimensions/pixels inspected. |
| `test/image_cache_decode/fixtures/pattern.png` | 1879 / `cc9c06ff4e00cbaf5266fe1edbba88abbc5d16c33bf8585722f486b1f7737527` | Synthetic pattern provenance and codec goldens read; decoded mode/dimensions/pixels inspected. |
| `test/manga_cover/fixtures/baseline.jpg` | 6684 / `e8fffa02fd0d8416f7afc05380f3787682fcb41765da2cb8d59090ff5fa2cc59` | Synthetic pattern provenance and codec goldens read; decoded mode/dimensions/pixels inspected. |
| `test/manga_cover/fixtures/gray.png` | 905 / `6592395e2296835fb7797300e41d8fe9eb31d85e725865cc1ee5ed9e4b851796` | Synthetic pattern provenance and codec goldens read; decoded mode/dimensions/pixels inspected. |
| `test/manga_cover/fixtures/mono.bmp` | 1502 / `48ac0b9c2851d94b75d08294e6472ff02457b980a8dee1d91d1ecd8b1c27d493` | Synthetic pattern provenance and codec goldens read; decoded mode/dimensions/pixels inspected. |
| `test/manga_cover/fixtures/palette.png` | 1685 / `ac26bd46fe01044c7db45552c30922f44b3630762f5795714d7b6e601dea5d4d` | Synthetic pattern provenance and codec goldens read; decoded mode/dimensions/pixels inspected. |
| `test/manga_cover/fixtures/progressive.jpg` | 6037 / `a908e6f5402cccdb4f8c1d5d1bae8756c277707da30572472b6b794f96b11c1f` | Synthetic pattern provenance and codec goldens read; decoded mode/dimensions/pixels inspected. |
| `test/manga_cover/fixtures/rgba.png` | 38598 / `64cfbed427e65fa3318551c354dbce50aeffa36a2890c4cb590b2eb26a105f80` | Synthetic pattern provenance and codec goldens read; decoded mode/dimensions/pixels inspected. |
| `test/manga_format/fixtures/meta-language.bin` | 24 / `d246ba6d66cbbac5df4199f846f2ed2b73cafee25df018363c808b4683f73d94` | Original pinned Python writer; explicit LE fields and literal UTF-8 inspected. |
| `test/manga_format/fixtures/meta-legacy.bin` | 20 / `4636b5f66cf9bfa0fef79cc8d3bde0f5330eefe64f656ed5121f90f977dee621` | Original pinned Python writer; explicit LE fields and literal UTF-8 inspected. |
| `test/manga_format/fixtures/panels.dat` | 89 / `af8f7830b81359e9a4be94f5e5d4301697b70e02db36604db656bf770ee27aaf` | Original pinned Python writer; explicit LE fields and literal UTF-8 inspected. |
| `test/manga_format/fixtures/panels.idx` | 44 / `112943c4e9ee2ae843af60772e2a02a241b0db0095ac117ce7f05e50971e448f` | Original pinned Python writer; explicit LE fields and literal UTF-8 inspected. |
| `test/manga_format/fixtures/toc.idx` | 49 / `0709e924aef13dd156ff0ebfe1d2a4d6abda433b4bf106cfd78085206a83dd12` | Original pinned Python writer; explicit LE fields and literal UTF-8 inspected. |

### Per-file manifest coverage — all 363 entries

Every text entry below has its patch-header line recorded. Implementation, tests
and public docs are fully reviewed; binary entries have the separate verification
above. Planning rows are explicitly contextual, not claims of complete rereading.

#### implementation (170)

| File | Coverage |
| --- | --- |
| `lib/CooperativeCancellation/CheckedPrint.h` | Entire text delta reviewed; implementation.patch:3. |
| `lib/CooperativeCancellation/CooperativeCancellation.h` | Entire text delta reviewed; implementation.patch:26. |
| `lib/Dict/DictIndex.cpp` | Entire text delta reviewed; implementation.patch:41. |
| `lib/Dict/DictIndex.h` | Entire text delta reviewed; implementation.patch:167. |
| `lib/Dict/DictionaryScanIdentity.h` | Entire text delta reviewed; implementation.patch:207. |
| `lib/Epub/Epub/converters/ImageToFramebufferDecoder.h` | Entire text delta reviewed; implementation.patch:307. |
| `lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp` | Entire text delta reviewed; implementation.patch:374. |
| `lib/Epub/Epub/converters/JpegToFramebufferConverter.h` | Entire text delta reviewed; implementation.patch:814. |
| `lib/Epub/Epub/converters/PixelCache.h` | Entire text delta reviewed; implementation.patch:854. |
| `lib/Epub/Epub/converters/PngToFramebufferConverter.cpp` | Entire text delta reviewed; implementation.patch:1065. |
| `lib/Epub/Epub/converters/PngToFramebufferConverter.h` | Entire text delta reviewed; implementation.patch:1390. |
| `lib/GfxRenderer/Bitmap.cpp` | Entire text delta reviewed; implementation.patch:1429. |
| `lib/GfxRenderer/Bitmap.h` | Entire text delta reviewed; implementation.patch:1485. |
| `lib/GfxRenderer/BitmapHelpers.h` | Entire text delta reviewed; implementation.patch:1503. |
| `lib/GfxRenderer/BmpConversionDimensions.h` | Entire text delta reviewed; implementation.patch:1717. |
| `lib/I18n/translations/english.yaml` | Entire text delta reviewed; implementation.patch:1727. |
| `lib/JpegToBmpConverter/JpegToBmpConverter.cpp` | Entire text delta reviewed; implementation.patch:1769. |
| `lib/JpegToBmpConverter/JpegToBmpConverter.h` | Entire text delta reviewed; implementation.patch:2117. |
| `lib/MangaPanel/LICENSE` | Entire text delta reviewed; implementation.patch:2149. |
| `lib/MangaPanel/MangaBitmapPixels.cpp` | Entire text delta reviewed; implementation.patch:2174. |
| `lib/MangaPanel/MangaBitmapPixels.h` | Entire text delta reviewed; implementation.patch:2377. |
| `lib/MangaPanel/MangaBook.cpp` | Entire text delta reviewed; implementation.patch:2412. |
| `lib/MangaPanel/MangaBook.h` | Entire text delta reviewed; implementation.patch:2856. |
| `lib/MangaPanel/MangaCover.cpp` | Entire text delta reviewed; implementation.patch:2925. |
| `lib/MangaPanel/MangaCover.h` | Entire text delta reviewed; implementation.patch:3374. |
| `lib/MangaPanel/MangaFormat.cpp` | Entire text delta reviewed; implementation.patch:3408. |
| `lib/MangaPanel/MangaFormat.h` | Entire text delta reviewed; implementation.patch:3598. |
| `lib/MangaPanel/MangaImageGeometry.cpp` | Entire text delta reviewed; implementation.patch:3685. |
| `lib/MangaPanel/MangaImageGeometry.h` | Entire text delta reviewed; implementation.patch:3768. |
| `lib/MangaPanel/MangaPendingInput.h` | Entire text delta reviewed; implementation.patch:3820. |
| `lib/MangaPanel/MangaPixelCache.cpp` | Entire text delta reviewed; implementation.patch:3857. |
| `lib/MangaPanel/MangaPixelCache.h` | Entire text delta reviewed; implementation.patch:4151. |
| `lib/MangaPanel/MangaPrefetchState.h` | Entire text delta reviewed; implementation.patch:4227. |
| `lib/PngToBmpConverter/PngToBmpConverter.cpp` | Entire text delta reviewed; implementation.patch:4268. |
| `lib/PngToBmpConverter/PngToBmpConverter.h` | Entire text delta reviewed; implementation.patch:4637. |
| `lib/hal/CheckedDirectoryEof.h` | Entire text delta reviewed; implementation.patch:4666. |
| `lib/hal/HalStorage.cpp` | Entire text delta reviewed; implementation.patch:4684. |
| `lib/hal/HalStorage.h` | Entire text delta reviewed; implementation.patch:4800. |
| `platformio.ini` | Entire text delta reviewed; implementation.patch:4849. |
| `scripts/patch_simulator_storage.py` | Entire text delta reviewed; implementation.patch:4895. |
| `scripts/pool_builtin_fonts.py` | Entire text delta reviewed; implementation.patch:4948. |
| `scripts/pool_builtin_fonts_pio.py` | Entire text delta reviewed; implementation.patch:5109. |
| `scripts/run_manga_simulator_smoke_test.py` | Entire text delta reviewed; implementation.patch:5128. |
| `src/BookmarkMutation.cpp` | Entire text delta reviewed; implementation.patch:5618. |
| `src/BookmarkStore.cpp` | Entire text delta reviewed; implementation.patch:5694. |
| `src/BookmarkStore.h` | Entire text delta reviewed; implementation.patch:5850. |
| `src/CrossPointState.cpp` | Entire text delta reviewed; implementation.patch:5912. |
| `src/CrossPointState.h` | Entire text delta reviewed; implementation.patch:5972. |
| `src/MappedInputManager.cpp` | Entire text delta reviewed; implementation.patch:5995. |
| `src/RecentBooksStore.cpp` | Entire text delta reviewed; implementation.patch:6042. |
| `src/RecentBooksStore.h` | Entire text delta reviewed; implementation.patch:6230. |
| `src/SettingsList.h` | Entire text delta reviewed; implementation.patch:6254. |
| `src/activities/Activity.h` | Entire text delta reviewed; implementation.patch:6291. |
| `src/activities/ActivityManager.cpp` | Entire text delta reviewed; implementation.patch:6320. |
| `src/activities/ActivityManager.h` | Entire text delta reviewed; implementation.patch:6549. |
| `src/activities/BackgroundSuspension.h` | Entire text delta reviewed; implementation.patch:6574. |
| `src/activities/boot_sleep/SleepActivity.cpp` | Entire text delta reviewed; implementation.patch:6596. |
| `src/activities/boot_sleep/SleepActivity.h` | Entire text delta reviewed; implementation.patch:6794. |
| `src/activities/boot_sleep/SleepCoverAssets.cpp` | Entire text delta reviewed; implementation.patch:6834. |
| `src/activities/boot_sleep/SleepCoverAssets.h` | Entire text delta reviewed; implementation.patch:7107. |
| `src/activities/boot_sleep/SleepCoverBudget.h` | Entire text delta reviewed; implementation.patch:7145. |
| `src/activities/home/BookActions.cpp` | Entire text delta reviewed; implementation.patch:7203. |
| `src/activities/home/BookActions.h` | Entire text delta reviewed; implementation.patch:7427. |
| `src/activities/home/FileBrowserActivity.cpp` | Entire text delta reviewed; implementation.patch:7449. |
| `src/activities/home/HomeActivity.cpp` | Entire text delta reviewed; implementation.patch:7782. |
| `src/activities/home/HomeActivity.h` | Entire text delta reviewed; implementation.patch:8190. |
| `src/activities/home/MangaCoverInput.h` | Entire text delta reviewed; implementation.patch:8248. |
| `src/activities/home/MangaCoverWork.h` | Entire text delta reviewed; implementation.patch:8282. |
| `src/activities/home/RecentBookProgress.cpp` | Entire text delta reviewed; implementation.patch:8345. |
| `src/activities/home/RecentBooksActivity.cpp` | Entire text delta reviewed; implementation.patch:8405. |
| `src/activities/home/RecentBooksGridActivity.cpp` | Entire text delta reviewed; implementation.patch:8490. |
| `src/activities/home/RecentBooksGridActivity.h` | Entire text delta reviewed; implementation.patch:8766. |
| `src/activities/network/CalibreConnectActivity.h` | Entire text delta reviewed; implementation.patch:8816. |
| `src/activities/network/CrossPointWebServerActivity.h` | Entire text delta reviewed; implementation.patch:8833. |
| `src/activities/network/NearbyStatsProtocol.cpp` | Entire text delta reviewed; implementation.patch:8848. |
| `src/activities/network/NearbyStatsProtocol.h` | Entire text delta reviewed; implementation.patch:9014. |
| `src/activities/network/NearbyStatsSyncActivity.cpp` | Entire text delta reviewed; implementation.patch:9069. |
| `src/activities/network/NearbyStatsSyncActivity.h` | Entire text delta reviewed; implementation.patch:9545. |
| `src/activities/reader/BookReadingStats.cpp` | Entire text delta reviewed; implementation.patch:9599. |
| `src/activities/reader/BookReadingStats.h` | Entire text delta reviewed; implementation.patch:9905. |
| `src/activities/reader/BookStatsActivity.cpp` | Entire text delta reviewed; implementation.patch:9967. |
| `src/activities/reader/BookStatsActivity.h` | Entire text delta reviewed; implementation.patch:10344. |
| `src/activities/reader/BookStatsView.cpp` | Entire text delta reviewed; implementation.patch:10440. |
| `src/activities/reader/BookStatsView.h` | Entire text delta reviewed; implementation.patch:10534. |
| `src/activities/reader/DictionaryScanIdentityPolicy.h` | Entire text delta reviewed; implementation.patch:10550. |
| `src/activities/reader/EpubLookupRequest.h` | Entire text delta reviewed; implementation.patch:10582. |
| `src/activities/reader/EpubReaderActivity.cpp` | Entire text delta reviewed; implementation.patch:10639. |
| `src/activities/reader/EpubReaderActivity.h` | Entire text delta reviewed; implementation.patch:10864. |
| `src/activities/reader/EpubReaderWordLookupActivity.cpp` | Entire text delta reviewed; implementation.patch:10889. |
| `src/activities/reader/EpubReaderWordLookupActivity.h` | Entire text delta reviewed; implementation.patch:11529. |
| `src/activities/reader/GlobalReadingStats.cpp` | Entire text delta reviewed; implementation.patch:11655. |
| `src/activities/reader/GlobalReadingStats.h` | Entire text delta reviewed; implementation.patch:12049. |
| `src/activities/reader/MangaMenuState.h` | Entire text delta reviewed; implementation.patch:12112. |
| `src/activities/reader/MangaNavigation.cpp` | Entire text delta reviewed; implementation.patch:12194. |
| `src/activities/reader/MangaNavigation.h` | Entire text delta reviewed; implementation.patch:12301. |
| `src/activities/reader/MangaPageTextSource.cpp` | Entire text delta reviewed; implementation.patch:12341. |
| `src/activities/reader/MangaPageTextSource.h` | Entire text delta reviewed; implementation.patch:12652. |
| `src/activities/reader/MangaPrefetch.cpp` | Entire text delta reviewed; implementation.patch:12694. |
| `src/activities/reader/MangaPrefetch.h` | Entire text delta reviewed; implementation.patch:12929. |
| `src/activities/reader/MangaPrefetchJpegBudget.cpp` | Entire text delta reviewed; implementation.patch:12987. |
| `src/activities/reader/MangaPrefetchPngBudget.cpp` | Entire text delta reviewed; implementation.patch:12997. |
| `src/activities/reader/MangaProgressStore.cpp` | Entire text delta reviewed; implementation.patch:13006. |
| `src/activities/reader/MangaProgressStore.h` | Entire text delta reviewed; implementation.patch:13193. |
| `src/activities/reader/MangaQrPayload.cpp` | Entire text delta reviewed; implementation.patch:13225. |
| `src/activities/reader/MangaQrPayload.h` | Entire text delta reviewed; implementation.patch:13320. |
| `src/activities/reader/MangaReaderActivity.cpp` | Entire text delta reviewed; implementation.patch:13333. |
| `src/activities/reader/MangaReaderActivity.h` | Entire text delta reviewed; implementation.patch:14813. |
| `src/activities/reader/MangaReaderSelectionActivity.cpp` | Entire text delta reviewed; implementation.patch:14974. |
| `src/activities/reader/MangaReaderSelectionActivity.h` | Entire text delta reviewed; implementation.patch:15223. |
| `src/activities/reader/MangaStatsCommit.cpp` | Entire text delta reviewed; implementation.patch:15282. |
| `src/activities/reader/MangaStatsCommit.h` | Entire text delta reviewed; implementation.patch:15339. |
| `src/activities/reader/MangaStatus.cpp` | Entire text delta reviewed; implementation.patch:15366. |
| `src/activities/reader/MangaStatus.h` | Entire text delta reviewed; implementation.patch:15389. |
| `src/activities/reader/MangaTranslationActivity.cpp` | Entire text delta reviewed; implementation.patch:15415. |
| `src/activities/reader/MangaTranslationActivity.h` | Entire text delta reviewed; implementation.patch:15520. |
| `src/activities/reader/MangaTranslationPager.h` | Entire text delta reviewed; implementation.patch:15560. |
| `src/activities/reader/PageTextSource.h` | Entire text delta reviewed; implementation.patch:15651. |
| `src/activities/reader/QrDisplayActivity.cpp` | Entire text delta reviewed; implementation.patch:15716. |
| `src/activities/reader/QrDisplayActivity.h` | Entire text delta reviewed; implementation.patch:15805. |
| `src/activities/reader/ReaderActivity.cpp` | Entire text delta reviewed; implementation.patch:15855. |
| `src/activities/reader/ReaderOptionsActivity.cpp` | Entire text delta reviewed; implementation.patch:15908. |
| `src/activities/reader/ReaderOptionsActivity.h` | Entire text delta reviewed; implementation.patch:15998. |
| `src/activities/reader/ReaderUtils.h` | Entire text delta reviewed; implementation.patch:16054. |
| `src/activities/reader/ReadingLanguageStats.cpp` | Entire text delta reviewed; implementation.patch:16086. |
| `src/activities/reader/ReadingLanguageStats.h` | Entire text delta reviewed; implementation.patch:16568. |
| `src/activities/reader/ReadingStatsSave.cpp` | Entire text delta reviewed; implementation.patch:16627. |
| `src/activities/reader/ReadingStatsSave.h` | Entire text delta reviewed; implementation.patch:16660. |
| `src/activities/reader/StatsBackup.cpp` | Entire text delta reviewed; implementation.patch:16695. |
| `src/activities/reader/XtcReaderActivity.cpp` | Entire text delta reviewed; implementation.patch:16851. |
| `src/activities/util/FullScreenMessageActivity.cpp` | Entire text delta reviewed; implementation.patch:16997. |
| `src/activities/util/FullScreenMessageActivity.h` | Entire text delta reviewed; implementation.patch:17037. |
| `src/components/OptionPopup.h` | Entire text delta reviewed; implementation.patch:17070. |
| `src/main.cpp` | Entire text delta reviewed; implementation.patch:17125. |
| `src/network/CrossPointWebServer.cpp` | Entire text delta reviewed; implementation.patch:17311. |
| `src/network/CrossPointWebServer.h` | Entire text delta reviewed; implementation.patch:17574. |
| `src/network/UsbSerialFileTransfer.cpp` | Entire text delta reviewed; implementation.patch:17596. |
| `src/network/WebDAVHandler.cpp` | Entire text delta reviewed; implementation.patch:17765. |
| `src/simulator/MangaStatusSmoke.cpp` | Entire text delta reviewed; implementation.patch:17877. |
| `src/simulator/MangaStatusSmoke.h` | Entire text delta reviewed; implementation.patch:17974. |
| `src/simulator/SimulatorSmokeTest.cpp` | Entire text delta reviewed; implementation.patch:17985. |
| `src/util/BookCacheUtils.cpp` | Entire text delta reviewed; implementation.patch:19745. |
| `src/util/BookDeletionSnapshot.cpp` | Entire text delta reviewed; implementation.patch:19865. |
| `src/util/BookDeletionSnapshot.h` | Entire text delta reviewed; implementation.patch:20020. |
| `src/util/BookFolderMutation.cpp` | Entire text delta reviewed; implementation.patch:20075. |
| `src/util/BookFolderMutation.h` | Entire text delta reviewed; implementation.patch:21229. |
| `src/util/BookMutationJournal.cpp` | Entire text delta reviewed; implementation.patch:21259. |
| `src/util/BookMutationJournal.h` | Entire text delta reviewed; implementation.patch:21441. |
| `src/util/BookMutationJson.cpp` | Entire text delta reviewed; implementation.patch:21502. |
| `src/util/BookMutationJson.h` | Entire text delta reviewed; implementation.patch:21857. |
| `src/util/BookMutationJsonAllocator.cpp` | Entire text delta reviewed; implementation.patch:21879. |
| `src/util/BookMutationJsonAllocator.h` | Entire text delta reviewed; implementation.patch:21925. |
| `src/util/BookMutationOwners.cpp` | Entire text delta reviewed; implementation.patch:21946. |
| `src/util/BookMutationOwners.h` | Entire text delta reviewed; implementation.patch:21993. |
| `src/util/BookMutationStorage.cpp` | Entire text delta reviewed; implementation.patch:22010. |
| `src/util/BookMutationStorage.h` | Entire text delta reviewed; implementation.patch:22293. |
| `src/util/DictionaryEngine.cpp` | Entire text delta reviewed; implementation.patch:22322. |
| `src/util/DictionaryEngine.h` | Entire text delta reviewed; implementation.patch:22364. |
| `src/util/JapaneseDictionaryBackend.h` | Entire text delta reviewed; implementation.patch:22403. |
| `src/util/QrCodePolicy.h` | Entire text delta reviewed; implementation.patch:22438. |
| `src/util/QrUtils.cpp` | Entire text delta reviewed; implementation.patch:22472. |
| `src/util/QrUtils.h` | Entire text delta reviewed; implementation.patch:22594. |
| `src/util/ScreenshotInfo.h` | Entire text delta reviewed; implementation.patch:22620. |
| `src/util/ScreenshotUtil.cpp` | Entire text delta reviewed; implementation.patch:22638. |
| `src/util/ScreenshotUtil.h` | Entire text delta reviewed; implementation.patch:22720. |
| `src/util/StarDictBackend.cpp` | Entire text delta reviewed; implementation.patch:22740. |
| `src/util/StarDictBackend.h` | Entire text delta reviewed; implementation.patch:22911. |
| `tools/manga_convert/LICENSE` | Entire text delta reviewed; implementation.patch:22963. |
| `tools/manga_convert/convert_manga.py` | Entire text delta reviewed; implementation.patch:22988. |
| `tools/simulator-patches/checked-storage.json` | Entire text delta reviewed; implementation.patch:24548. |
| `tools/simulator-patches/checked-storage.patch` | Entire text delta reviewed; implementation.patch:24564. |
#### tests (128)

| File | Coverage |
| --- | --- |
| `test/CMakeLists.txt` | Entire text delta reviewed; tests.patch:5. |
| `test/book_deletion_snapshot/BookDeletionSnapshotTest.cpp` | Entire text delta reviewed; tests.patch:38. |
| `test/book_deletion_snapshot/CMakeLists.txt` | Entire text delta reviewed; tests.patch:139. |
| `test/book_folder_mutation/CMakeLists.txt` | Entire text delta reviewed; tests.patch:153. |
| `test/book_folder_mutation/CheckedDirectoryTest.cpp` | Entire text delta reviewed; tests.patch:180. |
| `test/book_folder_mutation/JournalTest.cpp` | Entire text delta reviewed; tests.patch:210. |
| `test/book_folder_mutation/JsonTest.cpp` | Entire text delta reviewed; tests.patch:246. |
| `test/book_folder_mutation/MutationTest.cpp` | Entire text delta reviewed; tests.patch:317. |
| `test/book_folder_mutation/OwnerStubs.cpp` | Entire text delta reviewed; tests.patch:757. |
| `test/book_folder_mutation/stubs/HalStorage.cpp` | Entire text delta reviewed; tests.patch:778. |
| `test/book_folder_mutation/stubs/HalStorage.h` | Entire text delta reviewed; tests.patch:950. |
| `test/book_folder_mutation/stubs/Logging.h` | Entire text delta reviewed; tests.patch:1035. |
| `test/book_folder_mutation/test_simulator_patch.py` | Entire text delta reviewed; tests.patch:1043. |
| `test/builtin_font_pool/CMakeLists.txt` | Entire text delta reviewed; tests.patch:1102. |
| `test/builtin_font_pool/test_pool.py` | Entire text delta reviewed; tests.patch:1111. |
| `test/image_cache_decode/CMakeLists.txt` | Entire text delta reviewed; tests.patch:1273. |
| `test/image_cache_decode/ImageCacheDecodeTest.cpp` | Entire text delta reviewed; tests.patch:1299. |
| `test/image_cache_decode/fixtures/README.md` | Entire text delta reviewed; tests.patch:1506. |
| `test/image_cache_decode/fixtures/foreground-fnv64.txt` | Entire text delta reviewed; tests.patch:1523. |
| `test/image_cache_decode/fixtures/pattern.jpg` | Binary contract/hash/provenance verified above. |
| `test/image_cache_decode/fixtures/pattern.png` | Binary contract/hash/provenance verified above. |
| `test/image_cache_decode/stubs/Arduino.h` | Entire text delta reviewed; tests.patch:1544. |
| `test/image_cache_decode/stubs/FsHelpers.h` | Entire text delta reviewed; tests.patch:1556. |
| `test/image_cache_decode/stubs/GfxRenderer.h` | Entire text delta reviewed; tests.patch:1565. |
| `test/image_cache_decode/stubs/HalDisplay.h` | Entire text delta reviewed; tests.patch:1619. |
| `test/image_cache_decode/stubs/HalStorage.cpp` | Entire text delta reviewed; tests.patch:1623. |
| `test/image_cache_decode/stubs/HalStorage.h` | Entire text delta reviewed; tests.patch:1756. |
| `test/image_cache_decode/stubs/Logging.h` | Entire text delta reviewed; tests.patch:1822. |
| `test/image_cache_decode/stubs/MemoryBudget.h` | Entire text delta reviewed; tests.patch:1837. |
| `test/japanese_dictionary/CMakeLists.txt` | Entire text delta reviewed; tests.patch:1846. |
| `test/japanese_dictionary/JapaneseDictionaryTest.cpp` | Entire text delta reviewed; tests.patch:1884. |
| `test/japanese_dictionary/stubs/HalStorage.h` | Entire text delta reviewed; tests.patch:2870. |
| `test/manga_bitmap_pixels/CMakeLists.txt` | Entire text delta reviewed; tests.patch:3024. |
| `test/manga_bitmap_pixels/MangaBitmapPixelsTest.cpp` | Entire text delta reviewed; tests.patch:3035. |
| `test/manga_bitmap_pixels/stubs/HalStorage.cpp` | Entire text delta reviewed; tests.patch:3239. |
| `test/manga_bitmap_pixels/stubs/HalStorage.h` | Entire text delta reviewed; tests.patch:3301. |
| `test/manga_bitmap_pixels/stubs/Logging.h` | Entire text delta reviewed; tests.patch:3347. |
| `test/manga_book/CMakeLists.txt` | Entire text delta reviewed; tests.patch:3353. |
| `test/manga_book/MangaBookTest.cpp` | Entire text delta reviewed; tests.patch:3364. |
| `test/manga_book/stubs/HalStorage.cpp` | Entire text delta reviewed; tests.patch:3977. |
| `test/manga_book/stubs/HalStorage.h` | Entire text delta reviewed; tests.patch:4116. |
| `test/manga_book/stubs/Logging.h` | Entire text delta reviewed; tests.patch:4169. |
| `test/manga_converter/test_converter.py` | Entire text delta reviewed; tests.patch:4175. |
| `test/manga_cover/CMakeLists.txt` | Entire text delta reviewed; tests.patch:4361. |
| `test/manga_cover/MangaCoverTest.cpp` | Entire text delta reviewed; tests.patch:4396. |
| `test/manga_cover/RealCoverTest.cpp` | Entire text delta reviewed; tests.patch:4567. |
| `test/manga_cover/fixtures/README.md` | Entire text delta reviewed; tests.patch:5271. |
| `test/manga_cover/fixtures/baseline.jpg` | Binary contract/hash/provenance verified above. |
| `test/manga_cover/fixtures/goldens.txt` | Entire text delta reviewed; tests.patch:5308. |
| `test/manga_cover/fixtures/gray.png` | Binary contract/hash/provenance verified above. |
| `test/manga_cover/fixtures/mono.bmp` | Binary contract/hash/provenance verified above. |
| `test/manga_cover/fixtures/palette.png` | Binary contract/hash/provenance verified above. |
| `test/manga_cover/fixtures/progressive.jpg` | Binary contract/hash/provenance verified above. |
| `test/manga_cover/fixtures/rgba.png` | Binary contract/hash/provenance verified above. |
| `test/manga_cover/real_stubs/Arduino.h` | Entire text delta reviewed; tests.patch:5366. |
| `test/manga_cover/real_stubs/Arena.h` | Entire text delta reviewed; tests.patch:5378. |
| `test/manga_cover/real_stubs/FaultAllocation.h` | Entire text delta reviewed; tests.patch:5387. |
| `test/manga_cover/real_stubs/HalDisplay.h` | Entire text delta reviewed; tests.patch:5393. |
| `test/manga_cover/real_stubs/HalStorage.cpp` | Entire text delta reviewed; tests.patch:5403. |
| `test/manga_cover/real_stubs/HalStorage.h` | Entire text delta reviewed; tests.patch:5530. |
| `test/manga_cover/real_stubs/InflateStreamForTest.cpp` | Entire text delta reviewed; tests.patch:5597. |
| `test/manga_cover/real_stubs/Logging.h` | Entire text delta reviewed; tests.patch:5605. |
| `test/manga_cover/real_stubs/Print.h` | Entire text delta reviewed; tests.patch:5611. |
| `test/manga_cover/real_stubs/freertos/FreeRTOS.h` | Entire text delta reviewed; tests.patch:5626. |
| `test/manga_cover/real_stubs/freertos/task.h` | Entire text delta reviewed; tests.patch:5631. |
| `test/manga_cover/stubs/Converters.cpp` | Entire text delta reviewed; tests.patch:5636. |
| `test/manga_cover/stubs/HalStorage.cpp` | Entire text delta reviewed; tests.patch:5672. |
| `test/manga_cover/stubs/HalStorage.h` | Entire text delta reviewed; tests.patch:5791. |
| `test/manga_cover/stubs/JpegToBmpConverter.h` | Entire text delta reviewed; tests.patch:5852. |
| `test/manga_cover/stubs/Logging.h` | Entire text delta reviewed; tests.patch:5867. |
| `test/manga_cover/stubs/PngToBmpConverter.h` | Entire text delta reviewed; tests.patch:5873. |
| `test/manga_format/CMakeLists.txt` | Entire text delta reviewed; tests.patch:5885. |
| `test/manga_format/MangaFormatTest.cpp` | Entire text delta reviewed; tests.patch:5898. |
| `test/manga_format/MangaTranslationTest.cpp` | Entire text delta reviewed; tests.patch:6245. |
| `test/manga_format/fixtures/README.txt` | Entire text delta reviewed; tests.patch:6334. |
| `test/manga_format/fixtures/generate.py` | Entire text delta reviewed; tests.patch:6348. |
| `test/manga_format/fixtures/meta-language.bin` | Binary contract/hash/provenance verified above. |
| `test/manga_format/fixtures/meta-legacy.bin` | Binary contract/hash/provenance verified above. |
| `test/manga_format/fixtures/panels.dat` | Binary contract/hash/provenance verified above. |
| `test/manga_format/fixtures/panels.idx` | Binary contract/hash/provenance verified above. |
| `test/manga_format/fixtures/toc.idx` | Binary contract/hash/provenance verified above. |
| `test/manga_geometry/CMakeLists.txt` | Entire text delta reviewed; tests.patch:6397. |
| `test/manga_geometry/MangaImageGeometryTest.cpp` | Entire text delta reviewed; tests.patch:6415. |
| `test/manga_menu/CMakeLists.txt` | Entire text delta reviewed; tests.patch:6559. |
| `test/manga_menu/MangaBookmarkSaveTest.cpp` | Entire text delta reviewed; tests.patch:6623. |
| `test/manga_menu/MangaMenuTest.cpp` | Entire text delta reviewed; tests.patch:6679. |
| `test/manga_menu/MangaQrTest.cpp` | Entire text delta reviewed; tests.patch:6762. |
| `test/manga_menu/MangaScreenshotWriteTest.cpp` | Entire text delta reviewed; tests.patch:6832. |
| `test/manga_menu/MangaSmokeValidationTest.py` | Entire text delta reviewed; tests.patch:6864. |
| `test/manga_menu/MangaStatsCommitTest.cpp` | Entire text delta reviewed; tests.patch:6917. |
| `test/manga_menu/MangaStatusPixelsTest.cpp` | Entire text delta reviewed; tests.patch:7073. |
| `test/manga_menu/MangaStatusTest.cpp` | Entire text delta reviewed; tests.patch:7111. |
| `test/manga_menu/QrDrawTest.cpp` | Entire text delta reviewed; tests.patch:7135. |
| `test/manga_menu/QrPolicyTest.cpp` | Entire text delta reviewed; tests.patch:7169. |
| `test/manga_menu/screenshot_stubs/Arduino.h` | Entire text delta reviewed; tests.patch:7211. |
| `test/manga_menu/screenshot_stubs/FsHelpers.h` | Entire text delta reviewed; tests.patch:7217. |
| `test/manga_menu/screenshot_stubs/GfxRenderer.h` | Entire text delta reviewed; tests.patch:7225. |
| `test/manga_menu/screenshot_stubs/activities/Activity.h` | Entire text delta reviewed; tests.patch:7245. |
| `test/manga_menu/stubs/GfxRenderer.h` | Entire text delta reviewed; tests.patch:7254. |
| `test/manga_menu/stubs/components/themes/BaseTheme.h` | Entire text delta reviewed; tests.patch:7304. |
| `test/manga_navigation/CMakeLists.txt` | Entire text delta reviewed; tests.patch:7311. |
| `test/manga_navigation/MangaNavigationTest.cpp` | Entire text delta reviewed; tests.patch:7319. |
| `test/manga_pixel_cache/CMakeLists.txt` | Entire text delta reviewed; tests.patch:7434. |
| `test/manga_pixel_cache/MangaPixelCacheTest.cpp` | Entire text delta reviewed; tests.patch:7447. |
| `test/manga_prefetch/CMakeLists.txt` | Entire text delta reviewed; tests.patch:7740. |
| `test/manga_prefetch/MangaPrefetchTest.cpp` | Entire text delta reviewed; tests.patch:7776. |
| `test/manga_prefetch/MangaPrefetchWorkerTest.cpp` | Entire text delta reviewed; tests.patch:7868. |
| `test/manga_prefetch/stubs/Arduino.h` | Entire text delta reviewed; tests.patch:8044. |
| `test/manga_prefetch/stubs/FsHelpers.h` | Entire text delta reviewed; tests.patch:8057. |
| `test/manga_prefetch/stubs/Logging.h` | Entire text delta reviewed; tests.patch:8068. |
| `test/manga_prefetch/stubs/freertos/FreeRTOS.h` | Entire text delta reviewed; tests.patch:8077. |
| `test/manga_prefetch/stubs/freertos/task.h` | Entire text delta reviewed; tests.patch:8084. |
| `test/manga_progress/CMakeLists.txt` | Entire text delta reviewed; tests.patch:8109. |
| `test/manga_progress/MangaProgressStoreTest.cpp` | Entire text delta reviewed; tests.patch:8131. |
| `test/manga_progress/stubs/HalStorage.cpp` | Entire text delta reviewed; tests.patch:8273. |
| `test/manga_progress/stubs/HalStorage.h` | Entire text delta reviewed; tests.patch:8412. |
| `test/manga_progress/stubs/Logging.h` | Entire text delta reviewed; tests.patch:8476. |
| `test/reading_language_stats/CMakeLists.txt` | Entire text delta reviewed; tests.patch:8484. |
| `test/reading_language_stats/NearbyStatsProtocolTest.cpp` | Entire text delta reviewed; tests.patch:8504. |
| `test/reading_language_stats/ReadingLanguageStatsTest.cpp` | Entire text delta reviewed; tests.patch:8758. |
| `test/reading_language_stats/stubs/CrossPointSettings.h` | Entire text delta reviewed; tests.patch:9310. |
| `test/reading_language_stats/stubs/HalClock.h` | Entire text delta reviewed; tests.patch:9319. |
| `test/reading_language_stats/stubs/HalStorage.cpp` | Entire text delta reviewed; tests.patch:9328. |
| `test/reading_language_stats/stubs/HalStorage.h` | Entire text delta reviewed; tests.patch:9483. |
| `test/reading_language_stats/stubs/I18n.h` | Entire text delta reviewed; tests.patch:9562. |
| `test/reading_language_stats/stubs/Logging.h` | Entire text delta reviewed; tests.patch:9568. |
| `test/reading_language_stats/stubs/Print.h` | Entire text delta reviewed; tests.patch:9576. |
| `test/reading_language_stats/stubs/esp_mac.h` | Entire text delta reviewed; tests.patch:9591. |
#### documentation (9)

| File | Coverage |
| --- | --- |
| `CHANGELOG.md` | Entire text delta reviewed; documentation.patch:5. |
| `docs/file-formats.md` | Entire text delta reviewed; documentation.patch:56. |
| `docs/manga-bitmap-pixels.md` | Entire text delta reviewed; documentation.patch:268. |
| `docs/manga-covers.md` | Entire text delta reviewed; documentation.patch:284. |
| `docs/manga-format.md` | Entire text delta reviewed; documentation.patch:305. |
| `docs/manga-pixel-cache.md` | Entire text delta reviewed; documentation.patch:437. |
| `docs/manga-progress.md` | Entire text delta reviewed; documentation.patch:500. |
| `docs/manga-storage.md` | Entire text delta reviewed; documentation.patch:562. |
| `tools/manga_convert/README.md` | Entire text delta reviewed; documentation.patch:715. |
#### planning (55)

| File | Coverage |
| --- | --- |
| `docs/superpowers/plans/2026-09-04-unified-japanese-dictionary-port.md` | Context artifact indexed; planning.patch:3; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-05-manga-foundation-review.md` | Context artifact indexed; planning.patch:1112; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-05-manga-port-handoff.md` | Context artifact indexed; planning.patch:1198; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-05-manga-port-implementation.md` | Context artifact indexed; planning.patch:1443; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-05-manga-port-validation.md` | Context artifact indexed; planning.patch:1606; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-05-manga-reader-audit.md` | Context artifact indexed; planning.patch:2011; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-05-manga-reader-plan.md` | Context artifact indexed; planning.patch:2064; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-05-manga-reader-review.md` | Context artifact indexed; planning.patch:2118; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-05-manga-storage-plan.md` | Context artifact indexed; planning.patch:2179; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-05-manga-storage-review.md` | Context artifact indexed; planning.patch:2258; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-completion-gap-audit.md` | Context artifact indexed; planning.patch:2342; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-completion-plan.md` | Context artifact indexed; planning.patch:2388; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-cover-design.md` | Context artifact indexed; planning.patch:2524; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-font-pool-report.md` | Context artifact indexed; planning.patch:2719; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-language-stats-design.md` | Context artifact indexed; planning.patch:2856; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-library-audit.md` | Context artifact indexed; planning.patch:3144; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-library-completion-plan.md` | Context artifact indexed; planning.patch:3208; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-library-plan.md` | Context artifact indexed; planning.patch:3418; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-library-review.md` | Context artifact indexed; planning.patch:3445; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-menu-completion-audit.md` | Context artifact indexed; planning.patch:3468; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-ocr-audit.md` | Context artifact indexed; planning.patch:3537; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-ocr-plan.md` | Context artifact indexed; planning.patch:3674; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-ocr-source-report.md` | Context artifact indexed; planning.patch:3806; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-ocr-source-review.md` | Context artifact indexed; planning.patch:3860; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-ocr-ui-report.md` | Context artifact indexed; planning.patch:3896; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-ocr-ui-review.md` | Context artifact indexed; planning.patch:3998; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-pixel-audit.md` | Context artifact indexed; planning.patch:4046; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-pixel-plan.md` | Context artifact indexed; planning.patch:4108; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-pixel-review.md` | Context artifact indexed; planning.patch:4162; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-prefetch-audit.md` | Context artifact indexed; planning.patch:4209; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-prefetch-decode-report.md` | Context artifact indexed; planning.patch:4262; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-prefetch-plan.md` | Context artifact indexed; planning.patch:4386; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-prefetch-worker-report.md` | Context artifact indexed; planning.patch:4462; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-prefetch-worker-review.md` | Context artifact indexed; planning.patch:4707; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-scan-identity-design.md` | Context artifact indexed; planning.patch:4758; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-scan-identity-report.md` | Context artifact indexed; planning.patch:4822; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-scan-identity-review.md` | Context artifact indexed; planning.patch:5057; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-06-manga-transport-design.md` | Context artifact indexed; planning.patch:5101; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-07-manga-cover-completion-report.md` | Context artifact indexed; planning.patch:5227; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-07-manga-cover-completion-review.md` | Context artifact indexed; planning.patch:5534; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-07-manga-final-acceptance-index.md` | Context artifact indexed; planning.patch:5769; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-07-manga-final-known-findings.md` | Context artifact indexed; planning.patch:5827; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-07-manga-language-stats-report.md` | Context artifact indexed; planning.patch:5878; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-07-manga-language-stats-review.md` | Context artifact indexed; planning.patch:6355; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-07-manga-menu-api-preflight.md` | Context artifact indexed; planning.patch:6448; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-07-manga-menu-implementation-preflight.md` | Context artifact indexed; planning.patch:6509; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-07-manga-menu-report.md` | Context artifact indexed; planning.patch:6615; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-07-manga-menu-review.md` | Context artifact indexed; planning.patch:6819; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-07-manga-stats-persistence-preflight.md` | Context artifact indexed; planning.patch:6927; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-07-manga-stats-sync-preflight.md` | Context artifact indexed; planning.patch:7088; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-07-manga-transport-preflight.md` | Context artifact indexed; planning.patch:7126; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-07-manga-transport-report.md` | Context artifact indexed; planning.patch:7318; requirements/decisions reconciled above. |
| `docs/superpowers/plans/2026-09-07-manga-transport-review.md` | Context artifact indexed; planning.patch:7746; requirements/decisions reconciled above. |
| `docs/superpowers/specs/2026-09-04-unified-japanese-dictionary-port-design.md` | Context artifact indexed; planning.patch:7810; requirements/decisions reconciled above. |
| `docs/superpowers/specs/2026-09-05-manga-port-design.md` | Context artifact indexed; planning.patch:8126; requirements/decisions reconciled above. |
#### baseline-index (1)

| File | Coverage |
| --- | --- |
| `.codegraph/.gitignore` | Entire five-line local-index ignore policy read; preserve baseline. |
