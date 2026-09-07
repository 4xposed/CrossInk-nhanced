# Task 10c — manga folder transport and metadata recovery

Status: review-ready source at fix3 freeze. Targeted native tests and the first
successful C3 build are recorded below; root owns independent review and remaining
board/simulator gates. The pre-review self-audit findings were fixed, not accepted
as limitations: terminal output fingerprint verification, FAT case aliases of
durable cache filenames, Anki delete-cache parity, and an implicit temporary
string on the new snapshot API. No firmware was flashed by this worker.

## Scope and baseline

Existing WebDAV directory MOVE and USB RENAME now collect indexed manga beneath
the source, stage durable state, and migrate exact book identities after physical
movement. No new folder move/rename controls were added; HTTP file-only controls
and WebDAV empty-only directory DELETE remain. HTTP batch, USB and browser
recursive deletion use one bounded, deduplicated snapshot and confirmed-absence
metadata cleanup. Cache clearing retains both dictionary files and whole stats
primary/recovery files in place while removing disposable children.

The live baseline was captured after Task 10b's reviewed three-board gate in
`/private/tmp/crossink-manga-transport10c-before/` (410 files plus manifest/status),
before edits. English translation was captured separately before its first edit.
Preserved prior dictionary, rendering, cover, progress, language-stat and user
changes; no staging, commits, SDK/font edits, serial or firmware builds by this
worker. Root owns all PlatformIO/serial operations.

## Changed and new paths

Existing files changed by Task 10c, relative to that baseline:

- `platformio.ini`
- `test/CMakeLists.txt`
- `src/RecentBooksStore.cpp`
- `src/CrossPointState.cpp`
- `src/BookmarkStore.h`
- `src/RecentBooksStore.h`
- `src/BookmarkStore.cpp`
- `src/main.cpp`
- `src/CrossPointState.h`
- `src/util/BookCacheUtils.cpp`
- `src/util/BookDeletionSnapshot.h`
- `src/util/BookDeletionSnapshot.cpp`
- `src/network/CrossPointWebServer.cpp`
- `src/network/UsbSerialFileTransfer.cpp`
- `src/network/CrossPointWebServer.h`
- `src/network/WebDAVHandler.cpp`
- `src/activities/ActivityManager.cpp`
- `src/activities/ActivityManager.h`
- `src/activities/home/BookActions.cpp`
- `src/activities/home/FileBrowserActivity.cpp`
- `src/activities/util/FullScreenMessageActivity.h`
- `src/activities/util/FullScreenMessageActivity.cpp`
- `src/activities/network/CalibreConnectActivity.h`
- `src/activities/network/CrossPointWebServerActivity.h`
- `lib/hal/HalStorage.cpp`
- `lib/hal/HalStorage.h`
- `lib/I18n/translations/english.yaml`
- `test/book_deletion_snapshot/BookDeletionSnapshotTest.cpp`
- `test/manga_book/stubs/HalStorage.cpp`
- `test/manga_book/stubs/HalStorage.h`
- `CHANGELOG.md`
- `docs/file-formats.md`

New C++ implementation/tests:

- `src/util/BookMutationStorage.h`
- `src/util/BookMutationJson.cpp`
- `src/util/BookMutationJson.h`
- `src/util/BookMutationStorage.cpp`
- `src/util/BookMutationJournal.h`
- `src/util/BookMutationJournal.cpp`
- `src/util/BookMutationOwners.cpp`
- `src/util/BookMutationJsonAllocator.h`
- `src/util/BookMutationOwners.h`
- `src/util/BookMutationJsonAllocator.cpp`
- `src/util/BookFolderMutation.h`
- `src/util/BookFolderMutation.cpp`
- `src/BookmarkMutation.cpp`
- `lib/hal/CheckedDirectoryEof.h`
- `test/book_folder_mutation/CheckedDirectoryTest.cpp`
- `test/book_folder_mutation/JsonTest.cpp`
- `test/book_folder_mutation/MutationTest.cpp`
- `test/book_folder_mutation/OwnerStubs.cpp`
- `test/book_folder_mutation/JournalTest.cpp`
- `test/book_folder_mutation/stubs/HalStorage.cpp`
- `test/book_folder_mutation/stubs/HalStorage.h`
- `test/book_folder_mutation/stubs/Logging.h`

Additional new files: `test/book_folder_mutation/CMakeLists.txt`,
`test/book_folder_mutation/test_simulator_patch.py`,
`scripts/patch_simulator_storage.py`,
`tools/simulator-patches/checked-storage.patch`,
`tools/simulator-patches/checked-storage.json`, and this report. The actual used
simulator and sticky-simulator dependency mirrors contain exactly the carried
simulator-owned HAL patch; they are not app HAL shims.

## Contract and ownership

See `docs/file-formats.md` for exact CMJ1/CMP1/CMO1/CMI1/CMT1 framing, CRC and phase
values. The initial journal contract and its accepted review are in
`/private/tmp/crossink-manga-transport10c-staging/journal-layout.md` and the
reviewer's temp preflight report. Shared-original creation, sync/close/readback
and bounded owner validation precede PREPARED and DELETE_STARTED. Source-absent
is explicitly checked under the writer gate. Shared unknown fields stream
through unchanged; only matching paths/covers and confirmed-absent delete
references are rewritten. Cover-prefix matching requires a slash boundary.

Current/legacy bookmark owners stage independently and validate embedded source
path, version, count, lengths and exact record tail. Stats v6 files retain all
26,429 bytes in the maximum local fixture, including the day appendix; no stats
reencoding or global counter update occurs. Durable family limit counts recovery
siblings. Destination physical root, progress siblings, current/legacy bookmarks,
cache directories, recents and resume references are checked before physical
move. Source/destination hash aliases and loaded duplicate records are rejected.
Exact stored spelling of each source/destination ancestor is checked before hash
derivation. Uppercase PANELS.IDX is conservatively classified as indexed manga.

The main-task transaction gate first requests cancellation/draining through
ActivityManager's existing suspension contract. Active readers, stacked readers,
pending navigation or active uploads return Busy. No renderer/activity stack
mutation occurs in the transaction core; no RenderLock is held through journal,
copy, alias scans or worker waits. Recent/state writers, cache clear, stats reset
and new affected navigation are blocked while pending. Chunked I/O and scans
service `esp_task_wdt_reset()` and yield.

Boot recovery follows mounting/settings and precedes recent/app-state loading on
normal and network-resume routes. The completed journal remains under the sole
`.finalize` name while the snapshot/workspace are destroyed and owner stores
reload. Both checked reloads must succeed before final marker removal and gate
release. A failed reload retains the durable terminal journal.

Root approved a narrow explicit boot retry mode on FullScreenMessageActivity:
Confirm, Back, or a screen tap attempts recovery once; failure leaves responsive
input and the pending gate. The activity uses the existing exclusive-storage-loop
branch, blocks global input and auto-sleep, and is entered before setup returns.
GPIO and display/ActivityManager initialization have already completed. Success
logs and intentionally restarts to finish the normal initialization below the
recovery gate. No background retry loop and no boot content deletion. Live Home
allows USB explicit mutation retry; transfer activities service protocol retries.
WebDAV Overwrite:F collision maps to412 and Overwrite:T to409; Busy and pending
recovery map to503, with pending explicitly warning that physical mutation may
already have occurred. USB reports `ERR:metadata_recovery_pending`.

## Bounds and inherited limits

One fallible17,000-byte snapshot owns the16KiB path arena plus64 directories/books/root
indices. One reusable JsonScratch is3,840 bytes, including three1024-byte paths
and three256-byte byte/name buffers. Local deterministic stage paths are at most
96 bytes. No book decoder or growing manifest/file vector is allocated. Immutable
payload bound2,334,216 bytes is SD serialization only. Bookmark copy buffer256
bytes; full stats copy uses the same bounded chunk size. Unknown JSON nesting is
capped at8. Relevant path/label strings are at most1023 bytes and the original
recent known-string total is capped at16KiB before owner population; up to18 rows.
Normalized paths are at most1023 bytes; ambiguous255-byte getName results are
conservatively refused instead of assuming no truncation.

Owner JSON validation/reload uses a separate32KiB fallible allocator and rejects
source files over64KiB before parsing. It is cold owner work, not hidden transaction
workspace. Snapshot/workspace are released before final owner reload. Existing
RecentBook std::string/vector and state std::string population remains inherited
cold allocation behavior: standard-library allocation is not made fallible by a
bool wrapper. It is bounded by validated known strings but still carries that
existing OOM risk. No speculative free-heap check is claimed to guarantee success.

Pinned SdFat can return false on a FAT LFN checksum error with no parent read-error
bit. The checked app-HAL API therefore requires positive clean EOF: consumed zero
32-byte entry marker, or unchanged cursor with checked physical read returning0;
parent/card error, failed seek/restore, short read, wrapper OOM or ambiguous full
last directory slots reject the scan. Error is sticky until close/reopen; moved
state transfers the latch and clears the source. Ordinary openNextFile remains
compatible. This intentionally refuses some otherwise traversable full FAT
directories. Source/line evidence is in
`/private/tmp/crossink-manga-transport10c-enumeration-preflight.md`.

The simulator uses POSIX errno reset immediately before every readdir and reports
stat/child-open/close failures, preserving simulator ownership. The carried patch
is pinned to d07c68104f1de8e62a992e46250feeb5c6a22290 with exact pre/post hashes.
The active-environment-only prehook runs after dependency installation and before
compilation; unknown revision/content, mixed images or unsupported symlink
checkout stops the build. This is temporary consumer-carried integration under
the no-publish constraint: the clean published dependency itself does not contain
the patch. Evidence and actual PlatformIO6.1.19 timing source lines are in the
simulator packaging preflight under `/private/tmp`.

## Tests and evidence at first freeze

Commands (all disposable native fixtures):

```sh
cmake -S test -B /private/tmp/crossink-transport10c-tests -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/private/tmp/crossink-manga-pixel-build/_deps/googletest-src
cmake --build /private/tmp/crossink-transport10c-tests --target BookFolderMutationTest BookMutationJournalTest BookMutationJsonTest BookDeletionSnapshotTest CheckedDirectoryTest -j4
ctest --test-dir /private/tmp/crossink-transport10c-tests --output-on-failure -R 'Mutation|BookDeletion|CheckedDirectory'
python3 test/book_folder_mutation/test_simulator_patch.py
```

- Configure/build PASS: `/private/tmp/crossink-transport10c-configure.log`,
  `crossink-transport10c-all-build.log`.
- Initial native34/34 PASS,14.14 seconds: `crossink-transport10c-final-native.log`.
  Fix1 native36/36 PASS,25.84 seconds: `crossink-transport10c-fix1-native.log`.
  Final fix3 targeted38/38 PASS,14.44 seconds: `crossink-transport10c-fix3-native.log`.
- Single-I/O fault sweep tests every2054 call positions on an ancestor move;
  sources or destinations retain whole durable stats and file handles close.
  Earlier15-test sweep log: `crossink-transport10c-fault-sweep.log`.
- Move cuts cover each publication phase, false physical rename after movement,
  terminal owner reload failure/retry, CRC-valid redirected file records,
  unexpected terminal marker, physical/cache/recovery/recents collisions,
  direct/ancestor moves, separate bookmark families, progress siblings,
  dictionary/history, maximum v6 stats, uppercase index and16-file refusal.
- Delete tests cover recursive/overlapping root deduplication, USB depth/snapshot
  bounds, partial deletion retaining survivors, and recreated content during
  interrupted deletion (boot never resumes content deletion).
- JSON tests cover unknown20KiB values, cover-before-path ordering, Unicode
  escapes, row removal separators, truncation/overlong captured strings and
  trailing-comma corruption. HAL EOF policy cases cover valid marker/physical
  EOF, LFN-error last entry, deleted/full directory slots, parent/card/seek errors,
  alignment, and invalid-parent sticky latch reset.
- Simulator packaging4/4 PASS: `crossink-transport10c-patch-tests.log`; exact first
  application/idempotence, unknown revision/content, mixed images and a custom
  dependency directory. HEAD is mocked; no fixture Git commits exist.
- RED→GREEN evidence: snapshot late child-open failure
  `crossink-transport10c-scan-red.log`/`scan-green.log`; initial journal codec
  `journal-red.log`/`journal-green.log`; specialized recent trailing comma
  `json-red.log`/`json-green.log`; native enforcement of the device's single-open
  file rule exposed overlapping journal/stage-directory enumeration in
  `overlap-red.log` (2 failures), fixed by closing/restoring the journal reader
  before sibling probing, `overlap-green.log` (4 passes).

Paths above without leading directory are under `/private/tmp`. Native owner
hooks deliberately fake lifecycle and singleton reload; the ordinary-file cleanup
hook calls the real production filesystem helper, including Anki parity. They do not prove
FreeRTOS cancellation, real heap exhaustion or UI input behavior. The EOF tests
exercise the production policy plus native latch model, not actual FAT/card
fault hardware. Firmware and simulator compilation, target stack, board size and
hardware checks remain root-owned. The initial host .su audit found Snapshot
append656, Transaction load528, sharedOutput400 and recoverInternal336 bytes;
these are host frames only, not a claim about the C3's8192-byte main task.

## Remaining validation and hardware checks

Root-owned firmware gates so far:

- First C3 compile failed on `hasActiveUpload` referencing the cpp-local WS flag
  from an inline header method. Fixed by placing the method beside the actual
  WS state. Log `crossink-transport10c-c3-build.log`.
- Retry failed on incomplete Activity in the new owner boundary. Fixed with the
  actual Activity definition before ActivityManager, without a manager refactor.
  Log `crossink-transport10c-fix1-c3-build.log`.
- C3 fix2 PASS25.797 seconds,6,498,816 bytes,54,784 bytes free in the OTA partition;
  SHA256 `8298c0836b80e871d9bff6d72a4f6fbb5ee5e7112c2bda1d9e0bc0564ee60b41`.
  Log `crossink-transport10c-fix2-c3-build.log`. Fix3 parity/API edits follow that
  image; the final fix3 image follows.
- Final fix3 C3 PASS24.208 seconds,6,498,880 bytes,54,720 bytes free;
  SHA256 `b7f68d97f5af2899a7ccfdc2a9e26d62f6faed3f31fc111522f11810a373ba61`.
  Root simulator pair is building, log `crossink-transport10c-simulator-builds.log`.
- Root full native pre-fix suite PASS771/771 in44.97 seconds, log
  `crossink-transport10c-full-native-tests.log`.
- Root actual C3 GCC object prologues: Transaction load544, Snapshot append432,
  recoverDelete/create368, sharedOutput288, moveImpl/recoverInternal/
  publishReferences240, JSON rewrite entry176, owner validation128, bookmark
  staging128 bytes. Evidence: `crossink-transport10c-c3-stack-prologues.txt` and
  per-object assembly under `/private/tmp`. These are fixed per-function target
  reservations, not cumulative depth or runtime stack-watermark proof.

Fix1 snapshots are under `/private/tmp/crossink-transport10c-fix1-before`; fix2's
owner include preimage under `fix2-before`; fix3's Anki/helper/API/test preimages
under `fix3-before`. Two terminal-third-output/uppercase-durable regressions were
RED in `crossink-transport10c-fix1-red.log`, then4/4 focused GREEN in
`crossink-transport10c-fix1-green.log`. Anki cleanup was RED in
`crossink-transport10c-fix3-red.log`, then2/2 focused GREEN in `fix3-green.log`.
XTC/TXT cache behavior is explicitly retained and tested. All changed/new paths
remain the same as the list above; the real ordinary cleanup helper moved from
the application owner boundary into the existing task storage helper for testing.

Supplemental standalone allocation probe source is
`/private/tmp/crossink-transport10c-allocation-probe.cpp`, compiled with clang++
`-std=c++20 -Itest/book_folder_mutation/stubs -Isrc/util` and the existing
BookFolderMutationTest object files excluding MutationTest.cpp.o, output
`/private/tmp/crossink-transport10c-allocation-probe`; build and execution logs are
`crossink-transport10c-allocation-probe-build.log` and
`crossink-transport10c-allocation-probe.log`. The probe overrides nothrow allocation
for17,000/3,840/1,536-byte objects: snapshot/workspace fail safely, close all
handles, and accept later recovery; cache-clear workspace OOM refuses deletion.
It also uses an804-byte root with an injected first HAL open failure and counts
normal allocations: exactly the native HAL's own path string, with no additional
snapshot temporary. These are allocation-site tests, not simulated global
FreeRTOS heap fragmentation or claims about inherited owner std::string safety.
The probe has its own disposable fixture directory and no user SD access.

The prior Task10b C3 image6,467,888 bytes left85,712 bytes; the current task adds
30,992 bytes at final fix3. Upcoming10e still needs to fit. Final size/stack and simulator
results will be appended by root after review; no feature-cut claim is implied.

After independent review and target compilation, use only a copied disposable
indexed book on C3, Sticky and X4 Pro: set progress/panel mode/bookmarks, record
language stats and dictionary routing/history, move its ancestor over WebDAV and
USB, then reopen and compare state. Exercise both Overwrite modes with an occupied
destination. Interrupt after physical move and between shared publications; boot
must either finish metadata or retain a visible retry state, never delete content.
For partial recursive delete retain one unreadable/recreated book and verify its
metadata survives. Check Confirm/Back and touch retry on boot-pending state,
including repeated failure and successful intentional reboot. Measure main-task
stack high-water mark, internal heap/largest block and watchdog logs while moving
maximum stats and scanning64 directories; no unrelated user books or cache reset
is required. Pixel/thumbnail caches can regenerate; durable files must remain.

## Independent review fix round 1 — source frozen

The complete-package review identified exactly three Important findings. This
round changes only `src/util/BookFolderMutation.cpp`,
`src/util/BookDeletionSnapshot.cpp`,
`test/book_folder_mutation/MutationTest.cpp`, and
`test/book_deletion_snapshot/BookDeletionSnapshotTest.cpp`, plus this report.
All five preimages were captured before editing under
`/private/tmp/crossink-transport10c-review-fix1-before`.

Confirmed-deletion cleanup now removes every child of the owned manga cache,
including stats/dictionary/history, pixel/thumbnail/OCR/cover data, unknown
children and nested directories. Iterative postorder traversal reuses the
existing 1,024-byte path and 256-byte name fields inside the 3,840-byte transaction
workspace; it adds no allocation or recursive frame. Every child and parent
handle is explicitly closed and checked, enumeration/allocation failures stop
cleanup, and each completed directory is removed only after a clean checked
scan. Parent reopening stays within the exact cache root. Path overflow retains
recovery pending. Watchdog service remains at file and directory boundaries.
The final cache root must be confirmed absent before `DeleteMetadataDone` can
append. Recovery accepts an already absent cache without replaying content
removal.

Cleanup now rejects stage ordinals greater than or equal to 1,024. The test uses
CRC-valid CMI1 framing: ordinal 1,023 is removed and its pre-prepared operation
returns `MutationFailed` with no pending marker; ordinal 1,024 remains untouched
with `RecoveryPending`. Snapshot traversal now applies the depth limit only to
descending child directories. A fully checked depth-eight directory containing
files or an empty depth-eight directory is accepted; a deeper child directory
is rejected before physical mutation.

RED evidence: after adding five regression cases, the focused command below
failed all five against the reviewed implementation. Logs are
`/private/tmp/crossink-transport10c-review-fix1-red-build.log` and
`/private/tmp/crossink-transport10c-review-fix1-red.log`. An initial test enum-name
compile typo was corrected before this RED run. The first implementation run
passed four cases; the staging test's successful pre-prepared-abort expectation
was corrected from `Complete` to the existing `MutationFailed` result, retaining
its no-pending assertion and the failing invalid-ordinal protection.

```sh
cmake --build /private/tmp/crossink-transport10c-tests --target BookFolderMutationTest BookDeletionSnapshotTest -j4
ctest --test-dir /private/tmp/crossink-transport10c-tests --output-on-failure -R 'ConfirmedDelete|CacheScanFailure|StagingCleanup|UsbAccepts|DepthCeiling'
```

Final GREEN commands:

```sh
cmake --build /private/tmp/crossink-transport10c-tests --target BookFolderMutationTest BookMutationJournalTest BookMutationJsonTest BookDeletionSnapshotTest CheckedDirectoryTest -j4
ctest --test-dir /private/tmp/crossink-transport10c-tests --output-on-failure -R 'Mutation|BookDeletion|CheckedDirectory'
```

Build PASS with no warning/error in
`/private/tmp/crossink-transport10c-review-fix1-green-build.log`;
**43/43 tests PASS in 13.70s**, including the existing 2,054-position move fault
sweep, in `/private/tmp/crossink-transport10c-review-fix1-native.log`.
New deletion cases include a cut immediately before `DeleteMetadataDone`
(requiring the entire cache already absent), an unreadable nested child
(retaining the journal and original shared references until successful retry),
and unrelated-cache preservation. All paths close their test handles. No
simulator/HAL packaging changed in this round. Source is frozen for root's
review/build gates; the earlier C3 size is not a measurement of this new revision.

Hardware follow-up for this fix: on a disposable copied manga book, create
pixel/thumbnail/OCR files and a nested cache child, delete through HTTP or USB,
and verify the complete cache directory disappears. Interrupt storage during
nested cleanup and retry recovery; content must not be deleted again and shared
references must remain pending until cache absence is confirmed. Exercise USB
with files and an empty directory at depth eight, then with a depth-nine child;
only the latter should be refused before deletion. FAT enumeration/card-error
and cumulative task-stack caveats above still apply.

## Root integration after approved review fix1

Independent scoped re-review approved all three fixes with no new breakage.
`pio run -e default -e simulator -e sticky-simulator` passed in36.602 seconds
(23.152/6.732/6.718), log
`/private/tmp/crossink-transport10c-review-fix1-builds.log`. Final C3 image is
6,499,136 bytes, leaving54,464 bytes; SHA256
`82b99193a57f23e42a11815270bca7b3de3fd1a39057048f63e2fd2d5d82117c`.
The reviewed image/ELF are saved at `/private/tmp/crossink-task10c-firmware.*`
and have not been flashed. Both S3 board builds remain in progress at this entry.

Before the narrow review fixes, actual button and touch OCR/stats reader smokes
passed in34.236/34.505 seconds and EPUB regression in3.826 seconds. Their logs are
`/private/tmp/crossink-transport10c-{button,touch,epub}-smoke.log`.

After those fixes, the live localhost WebDAV/HTTP integration harness
`/private/tmp/crossink_transport10c_live_routes.py` passed with both
`--env simulator --port 23980` and `--env sticky-simulator --port 23982`.
It launches each already-built
profile in a fresh `/private/tmp` directory, creates only synthetic manga, and
enters the real transfer activity. Real MOVE requests preserved byte-identical
dictionary/history/opaque stats, recent/resume rewrites and unknown shared JSON
fields. Occupied destinations returned412/409 for Overwrite:F/T and retained
their bytes. WebDAV DELETE refused a nonempty directory with409; deduplicated
HTTP recursive deletion removed content, the whole nested cache and all pending
transaction markers. Logs: `crossink-transport10c-live-routes-button.log` and
`crossink-transport10c-live-routes-touch.log` under `/private/tmp`.

Disposable evidence remains at `/private/tmp/crossink-route10c-q44nz0f6` and
`/private/tmp/crossink-route10c-h1_xiqly`. The first harness attempt failed before
launch because its fixture import needed the repository scripts directory on
Python's import path; the harness was corrected before the passing runs.

The simulator runs HTTP callbacks on its listener thread, whereas hardware
services them synchronously through the main activity loop. These stationary,
single-client checks prove route/parser/files/store integration only; they do
not certify concurrent navigation, hardware owner synchronization or SD timing.
No user SD files or external network were used. The serial reader endpoint was
still absent at the latest enumeration.

Final S3 builds passed: Sticky202.098 seconds and X4Pro112.578 seconds,
combined314.677 seconds in `/private/tmp/crossink-transport10c-s3-builds.log`.
Sticky image6,278,512 bytes/free275,088, SHA256
`2e72f6fe51a5ac0b8e8e07e868d2378f77d7e7ba27832b737e926bbdc0323f0e`;
X4Pro image6,375,376 bytes/free178,224, SHA256
`d440ddd06abef08e061880d2cb0ee24e08199fb076bd469ec086c4156bd846c5`.
Task10c software implementation and task verification are complete, uncommitted.
The physical checks listed above remain outstanding.
