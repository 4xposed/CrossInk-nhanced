# Task 10b: bounded reading-language statistics

Date: 2026-09-07. Implemented in place on `matcha_features`, preserving the cover,
dictionary, manga, SDK and font work already present at dispatch. No staging,
commit, amendment or push was performed. This report is scoped against the
refreshed `/private/tmp/crossink-manga-stats10b-before` snapshot, not repository
HEAD (which also differs because of earlier tasks).

## Implemented behavior

- Fixed eight-entry, 64-byte language summaries: Unknown (`und`), Other (`mul`),
  six first-encounter normalized primary tags; bounded ASCII parsing, specified
  aliases, stable local slot identities and saturating language/day counters.
- Book v6 (137-byte summary) and global v4 (223-byte summary), followed locally
  by checked `LDAY` v1 rows. Full maximum files are 26,429 / 26,515 bytes. The
  writer streams at most 730 sorted calendar-day rows; no history matrix is
  embedded in copied snapshots. Null-span completion/date saves preserve the
  validated appendix byte-for-byte.
- Explicit v1–v5 book / v1–v3 global migration attributes old time to Unknown
  and invents no dates. v6/v5/v4/legacy book discovery is explicit. Invalid,
  truncated, oversized, newer, or failed-load snapshots cannot authorize an
  unsafe overwrite. Reads retain non-mutable provenance on transient second-pass
  failures. Full 64-bit file extents are checked before narrowing.
- Checked temp write/sync/close/validation/rename, per-file recovery, preservation
  of a validated backup when the primary is corrupt, and explicit fresh local
  reset. Cleanup failure after successful publication does not become a failed
  save. Backups stream the complete validated local file in a 64-byte buffer.
- EPUB, XTC and Manga use their existing accepted duration, threshold, timer and
  calendar start. XTC is Unknown; EPUB uses metadata and Manga uses `meta.bin`.
  Each already accepted span enters both language summaries once. A failed book
  or global target receives at most one immediate retry with the same snapshot
  and span; successful targets are skipped. No timer, journal or durable retry
  queue was added; TXT remains unchanged.
- Date/completion edits remain dirty until required writes publish. Preview
  reading seconds shown by EPUB/XTC BookStatsActivity are kept out of metadata
  saves so they cannot be committed early and added again on reader exit. EPUB
  completion feedback and downstream recents/move effects follow successful
  required saves. BookActions also stops its post-save effects on failure.
- Existing BookStatsActivity has language pages for book, local-device and
  all-device scopes. X4 retains its combined summary and gains language-page
  navigation; row pagination uses runtime height. Unknown/Other/empty/title
  strings are translated. Rendering uses resident summaries and bounded labels,
  without SD reads or new render-loop allocations. Home's existing stats entry
  now passes the matching manga/XTC cache path. The manga reader menu is unchanged.
- Nearby uses a shared production packet codec and session state machine. Byte7
  advertises maximum importable summary version; legacy zero means v3. Updated
  peers exchange 223-byte summaries only. An updated device sends no v4 summary
  to a legacy peer, durably imports/ACKs its supported summary, then reports
  mismatch. Send queue acceptance is never an import ACK. Peer MAC/source checks,
  capability consistency, early ACK rejection, retransmissions and lost-final-ACK
  handling retain bounded timeout/failure behavior. Aggregation selects six
  lexical tags deterministically, skips this device and invalid/full-local synced
  files, and returns local-only when local MAC lookup fails.

## Explicit limits

Daily history is local only. Nearby has no daily-language transport or remote
calendar aggregation. Supplied committed spans use the existing reader convention
that compresses paused/idle gaps, not wall-clock-exact session reconstruction.
Lifetime totals are not reduced when old rows expire. Existing EPUB/XTC total
counter overflow behavior is unchanged; new language counters saturate.

Book/global files remain separate transactions. A power loss between successful
file publications can leave temporary divergence. After the one immediate retry,
remaining errors are logged; no recovery queue persists the failed in-memory span.
Failed metadata edits remain interactive across normal Back, Home, navigation and
sleep attempts. Forced power loss still has the existing per-file boundary.

## Changed existing files

- `CHANGELOG.md`
- `docs/file-formats.md`
- `lib/I18n/translations/english.yaml`
- `scripts/run_manga_simulator_smoke_test.py`
- `src/activities/Activity.h`
- `src/activities/ActivityManager.h`
- `src/activities/ActivityManager.cpp`
- `src/activities/BackgroundSuspension.h`
- `src/main.cpp`
- `src/activities/home/BookActions.cpp`
- `src/activities/home/HomeActivity.cpp`
- `src/activities/network/NearbyStatsSyncActivity.cpp`
- `src/activities/network/NearbyStatsSyncActivity.h`
- `src/activities/reader/BookReadingStats.cpp`
- `src/activities/reader/BookReadingStats.h`
- `src/activities/reader/BookStatsActivity.cpp`
- `src/activities/reader/BookStatsActivity.h`
- `src/activities/reader/BookStatsView.cpp`
- `src/activities/reader/BookStatsView.h`
- `src/activities/reader/EpubReaderActivity.cpp`
- `src/activities/reader/EpubReaderActivity.h`
- `src/activities/reader/GlobalReadingStats.cpp`
- `src/activities/reader/GlobalReadingStats.h`
- `src/activities/reader/MangaReaderActivity.cpp`
- `src/activities/reader/StatsBackup.cpp`
- `src/activities/reader/XtcReaderActivity.cpp`
- `src/simulator/SimulatorSmokeTest.cpp`
- `test/CMakeLists.txt`

Additional baseline snapshots beyond root's original 30 files are
`scripts/run_manga_simulator_smoke_test.py`, `src/activities/Activity.h`,
`src/activities/ActivityManager.h`, `src/activities/ActivityManager.cpp`,
`src/activities/BackgroundSuspension.h`, and `src/main.cpp` (baseline now36 files).
Header snapshots were checked before editing. XTC/Manga headers are unchanged against that baseline.
Generated I18n outputs were regenerated through the generator, not hand edited;
no generated output is intended as a separate source change.

## New files

- `src/activities/reader/ReadingLanguageStats.h`
- `src/activities/reader/ReadingLanguageStats.cpp`
- `src/activities/reader/ReadingStatsSave.h`
- `src/activities/reader/ReadingStatsSave.cpp`
- `src/activities/network/NearbyStatsProtocol.h`
- `src/activities/network/NearbyStatsProtocol.cpp`
- `test/reading_language_stats/CMakeLists.txt`
- `test/reading_language_stats/ReadingLanguageStatsTest.cpp`
- `test/reading_language_stats/NearbyStatsProtocolTest.cpp`
- `test/reading_language_stats/stubs/CrossPointSettings.h`
- `test/reading_language_stats/stubs/HalClock.h`
- `test/reading_language_stats/stubs/HalStorage.h`
- `test/reading_language_stats/stubs/HalStorage.cpp`
- `test/reading_language_stats/stubs/I18n.h`
- `test/reading_language_stats/stubs/Logging.h`
- `test/reading_language_stats/stubs/Print.h`
- `test/reading_language_stats/stubs/esp_mac.h`
- This report, `docs/superpowers/plans/2026-09-07-manga-language-stats-report.md`.

## Agent verification and RED/GREEN evidence

Configuration (Darwin host):

```sh
cmake -S test -B /private/tmp/crossink-stats10b-build -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/private/tmp/crossink-manga-pixel-build/_deps/googletest-src
cmake --build /private/tmp/crossink-stats10b-build --target ReadingLanguageStatsTest -j4
/private/tmp/crossink-stats10b-build/reading_language_stats/ReadingLanguageStatsTest
```

Latest agent result: **38/38 passed, 41 ms**, exit 0, recorded in
`/private/tmp/stats10b-size-green.log`; build log
`/private/tmp/stats10b-size-green-build.log`. Target compilation enables
`-fstack-usage`. No broad suite, PIO, radio or serial command was run by this agent.
Initial googletest compilation emitted one upstream Apple Clang char8_t warning;
final touched production/test compilation was clean.

RED evidence:

1. `/private/tmp/stats10b-red.log`: both initial real-persistence tests failed on
   old production output: book v6 missing, global version3 and159 bytes instead
   of version4 and235 bytes. Both passed after implementation.
2. `/private/tmp/stats10b-size-red.log`: a file grows to 4GiB+235 between backup
   validation and copy, with the test HAL emulating 32-bit `fileSize()`. The real
   backup incorrectly returned true. After changing its second-open equality to
   `fileSize64()`, the test and all 38 tests pass. Exact command:

```sh
/private/tmp/crossink-stats10b-build/reading_language_stats/ReadingLanguageStatsTest --gtest_filter=LanguageStatsTest.BackupRejectsLargeExtentAppearingBetweenValidationAndCopy
```

Coverage includes all legacy versions; normalization and capacity overflow;
mixed metadata sessions; leap/month/year midnights; invalid RTC and zero spans;
rollback/window expiry; maximum byte-exact appendices; counter saturation;
summary-only recovery; truncations/malformed tags/versions/rows; failed writes,
syncs, closes and renames; corrupt-primary backup recovery; reset; second-pass
load failure provenance; deterministic aggregate/self-MAC failure; oversized sparse
files; full streamed backup; separate target retry and dirty metadata behavior.
The two-peer tests execute the actual codec/session/publication implementation,
including an exact frozen pre-v4 validator, all legacy summary versions, early
and spoofed ACKs, capability inconsistency, malformed envelopes, storage failures,
timeout, overflow, radio packet loss and retransmission after a lost final ACK.

Other agent checks:

```sh
python3 scripts/gen_i18n.py
python3 -m py_compile scripts/run_manga_simulator_smoke_test.py
git diff --check
```

All exited 0. Root owns final simulator/hardware and broad-suite evidence. After
the last one-line backup extent fix, root reported both simulator builds passing
and 740/740 broad native tests passing in 31.08s. Root also reported button
OCR+stats UI smoke passing in 33.143s and EPUB smoke passing in 3.757s. Touch
verified the actual language totals and navigation, then failed the Home assertion.
Hardware PIO exposed an unresolved zero-MAC helper reference before root stopped
the remaining builds. The scoped corrections below still require root rebuilds
and a touch rerun; previous successes do not validate those final corrections.

The simulator script now opens the actual BookStatsActivity with committed manga
stats plus a preview-only Unknown row, navigates via normal button or touch hints,
and compares the routed language-page framebuffer with the production view while
masking page dots. It returns to Home before the existing recents/reopen position
checks. Cache clearing also checks that v4/v5/v6 filenames and language seconds
survive. This uses the existing simulator direct-activity pattern; it does not add
product menu rows or test-only public methods. Root must run these flows before
claiming their runtime acceptance.

## Integration corrections after root verification

Pre-fix copies of `NearbyStatsSyncActivity.cpp` and `SimulatorSmokeTest.cpp` are
at `/private/tmp/crossink-stats10b-integration1-before`, preserving their repository
relative paths. The lifecycle correction below then added eleven more pre-fix copies in that
integration snapshot; its complete inventory is listed below.

- Hardware compilation found two rendering calls to removed `isZeroMac` at
  NearbyStatsSyncActivity lines 548/556. Both now directly compare the MAC array
  with an empty array, matching the existing protocol predicate. Simulator builds
  exclude that hardware branch, so only the root hardware rebuild can close this
  compile failure.
- Touch log `/private/tmp/crossink-stats10b-touch-smoke.log` showed Home entered at
  29.500s, BookStats pushed at 29.968s, and no BookStats exit before failure at
  32.029s. Earlier scripted logical Back releases bypass the simulator's pending
  reader Back-release suppression; the first real footer touch Back is swallowed.
  The touch fixture now uses the existing production header Back hit rectangle,
  with an explicit BookStats assertion and render log before exiting. Button Back
  coverage remains. Home is the pushed activity's parent, so a normal pop returns
  there; the constructor's optional return-to-Home flag is not the cause. Global
  input behavior was not changed.

Correction checks:

```sh
/private/tmp/crossink-stats10b-build/reading_language_stats/ReadingLanguageStatsTest --gtest_filter='NearbyProtocolTest.*'
python3 -m py_compile scripts/run_manga_simulator_smoke_test.py
git diff --check
```

Protocol tests: 9/9 pass (log `/private/tmp/stats10b-integration1-protocol.log`);
Python compilation and whitespace check exit 0. These checks do not replace the
pending hardware compilation and touch runtime rerun.

## Review correction: retain dirty edits across normal lifecycle exits

The review's High Home finding is fixed by a guarded `handleHomeGesture()` that
consumes failed attempts. Other normal transitions and explicit sleep now run the
same grouped metadata save in `prepareToSuspend()`. `cancelSuspensionOnFailure()`
defaults false for all existing activities and is true only for BookStats. A stats
failure clears that pending navigation or cancels sleep, returning to input;
background draining retains the existing pre-lock cancellation, locked readiness
check and pending retry behavior. No repeated SD save is added to the drain loop.

A failed edit latches failure until successful publication or a new explicit edit;
normal and Quick Lock automatic sleep respect that latch. Explicit Back/Home/power
attempts can retry. A translated banner, "Save failed. Check SD card and retry.",
keeps the editor controls available. `onExit()` no longer has to veto destruction:
the guarded transition has already succeeded before it runs.

The real RTC simulator flow now edits a date through normal input, obstructs the
global temp path with a nonempty directory, attempts Home, checks that BookStats
remains, attempts real `enterDeepSleep(false)`, checks that subsequent scripted
input still runs, removes the obstruction, retries Home, and verifies the retained
manual date persisted. It uses existing ActivityManager/sleep entry points and SD
operations, with no new public test seam. Root must execute this added flow.

Complete integration snapshot files (repository relative paths):

- `lib/I18n/translations/english.yaml`
- `src/activities/Activity.h`
- `src/activities/ActivityManager.h`
- `src/activities/ActivityManager.cpp`
- `src/activities/BackgroundSuspension.h`
- `src/activities/network/NearbyStatsSyncActivity.cpp`
- `src/activities/reader/BookStatsActivity.h`
- `src/activities/reader/BookStatsActivity.cpp`
- `src/activities/reader/ReadingStatsSave.h`
- `src/activities/reader/ReadingStatsSave.cpp`
- `src/main.cpp`
- `src/simulator/SimulatorSmokeTest.cpp`
- `test/reading_language_stats/ReadingLanguageStatsTest.cpp`

The native regression first failed to compile because the failure latch and
suspension retry policy were absent (`/private/tmp/stats10b-lifecycle-red.log`).
After implementation, these exact commands pass:

```sh
cmake --build /private/tmp/crossink-stats10b-build --target ReadingLanguageStatsTest -j4
/private/tmp/crossink-stats10b-build/reading_language_stats/ReadingLanguageStatsTest
python3 scripts/gen_i18n.py
python3 -m py_compile scripts/run_manga_simulator_smoke_test.py
git diff --check
```

Final focused result: **39/39 pass in40ms**, including the native shared gate
classification (background failure remains retryable, stats failure cancels) and
metadata failure latch/reset/per-target persistence assertions. Logs:
`/private/tmp/stats10b-lifecycle-green-build.log`,
`/private/tmp/stats10b-lifecycle-green.log`, and
`/private/tmp/stats10b-lifecycle-i18n.log`. The actual Home/sleep failure UI flow and
hardware branch remain pending root verification at this handoff. The review's
Low obsolete Nearby state cleanup is deliberately deferred from this scoped fix.

## Review correction round 2: canceled Quick Lock sleep intent

The Medium review finding is fixed in `src/main.cpp`: after failed preparation,
both Quick Lock resume fields clear only when the transition is canceled. A
background owner still draining retains its pending sleep intent. Successful
hardware sleep still persists the intended Quick Lock marker and never returns;
the existing simulator-return cleanup now clears both fields, only when sleep is
not still pending.

Only `src/main.cpp` and `src/simulator/SimulatorSmokeTest.cpp` changed in this
round. Their exact pre-fix copies are under
`/private/tmp/crossink-stats10b-fix2-before` with repository relative paths. They
were already included in the task baseline; no new file or baseline addition.

The actual BookStats failure smoke now arms the same resume fields as Quick Lock
before calling real `enterDeepSleep(true)` with the SD obstruction, and asserts
both clear on cancellation. After editor recovery and the existing reader-position
checks, it performs ordinary `enterDeepSleep(false)` and reloads the durable app
state; neither resume field may survive. It reuses the existing simulator sleep
preparation seam (`CROSSINK_SIMULATOR_MANGA_PREFETCH_STRESS`) for that final call,
so full preparation and state persistence execute without entering the HAL's
intentional wait-for-wake loop. This validates the persisted wake intent, not a
physical reboot or actual locked-input sequence; the latter remains hardware
verification.

Root reported fix1 touch/failure smoke PASS34.336s and EPUB smoke PASS3.767s.
The button failure reproduced at RecentBooksGrid's Confirm release. Its cause was
fixture sequencing: BookStats Confirm-release editor entry arms
`suppressNextConfirmRelease()`, and `MappedInputManager::simulatorInjectRelease()`
consumes that latch on the next synthetic Confirm. That next Confirm was the
recents reopen. The fixture now enters the editor through its existing Left-press
route, keeping production input behavior unchanged. This is not a timing delay.

Checks this round:

```sh
/private/tmp/crossink-stats10b-build/reading_language_stats/ReadingLanguageStatsTest
python3 -m py_compile scripts/run_manga_simulator_smoke_test.py
git diff --check
```

**39/39 focused tests pass in37ms** (including the retained background-drain policy
test), log `/private/tmp/stats10b-fix2-native.log`; other checks exit0. No new native
helper was introduced solely to test two field assignments. Final simulator
builds, both UI regressions and hardware builds remain root-owned at this freeze.

## Hardware integration correction round 3: fixed-width integer deduction

C3 compilation found that `uint32_t` is `unsigned long` on its toolchain, while
`1u` is `unsigned int`; deduced `std::max` therefore failed in `SpanDays` at
`ReadingLanguageStats.cpp:74`. That call now uses explicit `std::max<uint32_t>`.
The remaining new min/max calls were audited against the task snapshot and the
six new production files: they either use explicit template types or operands
with the same declared `int`, `size_t`, `uint32_t`, or `uint64_t` type. No other
portability change was needed.

Only `src/activities/reader/ReadingLanguageStats.cpp` changed. Its exact pre-fix
copy is `/private/tmp/crossink-stats10b-fix3-before/src/activities/reader/ReadingLanguageStats.cpp`.
No new task file or baseline addition. Root's failing hardware log is
`/private/tmp/crossink-stats10b-fix2-hardware-builds.log` (error after line3030).

```sh
cmake --build /private/tmp/crossink-stats10b-build --target ReadingLanguageStatsTest -j4
/private/tmp/crossink-stats10b-build/reading_language_stats/ReadingLanguageStatsTest
git diff --check
```

Focused result **39/39 pass in40ms**, with logs
`/private/tmp/stats10b-fix3-build.log` and `/private/tmp/stats10b-fix3-native.log`;
whitespace check exits0. This native result does not replace root's pending C3
compiler verification. Root reported fix2 review PASS and both actual simulator
smokes PASS (about34s) before this single typed-template correction.

## Bounded resources and stack evidence

Native `sizeof` measurement:

| Value | Bytes |
|---|---:|
| ReadingLanguageTotals | 64 |
| BookReadingStats | 148 |
| GlobalReadingStats | 228 |
| ReadingLanguageSpan | 16 |
| Nearby Event | 259 |
| Nearby Session | 28 |

Totals and global size have production static assertions. No local or member
array scales to730 days. Streamed history has one 36-byte input row and one 36-byte
output row, plus scalar interval bounds. Null-span copying uses 64 bytes; summary
coding uses 8-byte entries. New book/shared/Nearby transaction names have checked
96-byte stack buffers. Existing caller strings and HAL file handles remain cold
filesystem operations. No new heap allocation occurs per day row or language row.

The eight Nearby queued events grow by 65 bytes each versus the old 194-byte event
(520 bytes retained); the local wire summary grows 64 bytes and Session adds 28.
Its on-stack packet grows from 173 to 237 bytes. These costs exist only while the
Nearby activity exists. Reader book/global members grow by the fixed summaries
and small provenance flags; UI snapshot copies remain fixed-size. No fonts, SDK
or PSRAM-dependent allocation were added.

Apple Clang `-fstack-usage` reports (optimized native build):

| Function/path | Frame bytes |
|---|---:|
| streamDays | 256 |
| writeTemp | 240 |
| publishReadingLanguageFile | 208 |
| inspectReadingLanguageFile | 192 |
| visitLocalReadingLanguageDays | 240 |
| saveReadingStatsWithRetry | 80 |
| Book load / find / save | 336 / 320 / 272 |
| Global aggregatePass / loadFromOpenFile / reset | 528 / 272 / 288 |
| Nearby publishSummary | 336 |
| backupGlobalStats | 640 |

These are individual frames, **not cumulative call-chain high-water marks**.
Explicit cold helper boundaries keep the streamed writer's bounded scratch clear,
but do not establish a target stack margin. The native HAL stub stores a FILE
pointer, directory pointer and std::string; the real HAL instead stores its
implementation pointer and allocation flag, so the native numbers are not target
measurements. Filesystem callees and caller frames must also fit the actual task.
The unchanged backup-pruning libc++ sort reports 1120 bytes in one frame; that is
not a new history allocation and is not changed here. Hardware stack high-water
and heap measurements remain required; no measured target-memory/performance win
is claimed.

## Hardware verification still required

On an X4/C3, read an EPUB, XTC and manga for qualifying active intervals, pause in
menus/lookup, then exit and power-cycle. Their old total/page cadence should remain
unchanged; EPUB/Manga metadata names and XTC Unknown should match only the new
committed intervals. Change manga metadata between sessions and include an empty
language; prior named totals must remain attributed to their original slots.

On an RTC-capable target cross a known midnight/leap boundary, then inspect local
rows; roll the clock backward and verify newer rows remain. Clear a book cache,
reopen stats and preserve all totals; reset local device stats and verify both
local summary and appendix clear while remote snapshots/backups remain. No EPUB
content-cache reset is required for this stats-only format change.

Inject SD write/sync/rename failures where practical. Old data must remain
recoverable; logs distinguish book/global failure and the single retry must not
replay an already successful daily span. Exercise maximum histories while measuring
internal heap, largest free block and task stack high-water marks. Repeat on S3
and inspect PSRAM/internal pools separately.

Pair updated↔updated and updated↔legacy devices. Confirm local summary size 223,
no appendix transport, no duplicate self aggregation, and final ACK only after
remote publication. Old summaries should import then show version mismatch;
missing old payload may time out. Test lost final ACK and failed SD publication.
Navigate book/local/all language pages with buttons and touch, then return to the
same reading position.

## Root verification checkpoint after lifecycle corrections

Fix1 full native suite: `cmake --build build/task16-tests -j4` followed by
`ctest --test-dir build/task16-tests -j1 --output-on-failure`:741/741 pass40.36s.
Fix2 changes only main sleep-state handling and simulator fixture; both simulator
builds passed15.359s. Actual manga OCR/stats/save-failure workflows passed on
button34.248s and touch34.504s, including persisted ordinary-sleep state after
canceled stats sleep. EPUB flow passed3.767s before the marker-only correction.
Logs `/private/tmp/crossink-stats10b-fix{1,2}-*.log`.

The initial hardware group found a C3-only integer template mismatch; it was
stopped before completing S3 and provides no accepted firmware artifacts.
Fix3 is the reviewed explicit uint32_t max template argument; focused39/39 pass.
Fresh C3 then S3 verification is in progress. Reader serial endpoint remains
absent (only debug-console, Bluetooth, SonosAce and Px8S2); no new flash occurred.

C3 fix3 verification: `pio run -e default` exit0,114.466s. Final image
6,467,888 bytes; app partition6,553,600, remaining85,712. SHA256
`cc88746d352da4c900d0d6362d4e37deb8d01e6f94a19a6e78dd7cc75db3b25d`.
Saved reviewed checkpoint `/private/tmp/crossink-task10b-firmware.{bin,elf}`;
unflashed. S3 verification remains in progress. No new performance claim is
made from image size; no physical heap/stack measurements were available.

Final S3 verification: `pio run -e sticky -e x4-pro` exit0,303.269s combined.
Sticky189.203s:6,250,272 bytes/free303,328, SHA256
`f8590ce58fed746c2acd8c672d3bb21b1ad213df304f6727970c52ded6ea5dfe`.
X4Pro114.065s:6,347,088 bytes/free206,512, SHA256
`868ef3f35bc91152d368ee61795253265c06c6b16206ff8ccb3040e48451faea`.
All final board builds pass. Task10b software implementation/review gate complete;
physical language/session/SD-failure/Nearby acceptance remains unperformed.
