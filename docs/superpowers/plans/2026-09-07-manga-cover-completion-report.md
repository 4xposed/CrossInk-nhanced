# Task 10a: manga cover cancellation and allocation completion

Date: 2026-09-07. Implementation and focused native verification are complete; independent root review, full native suite,
simulator/firmware builds and hardware verification are separate gates. Source/test inputs are frozen for those builds. Root subsequently reported 693/693 native tests passing, simulator and
sticky-simulator builds passing, and normal manga, EPUB and touch OCR smoke checks passing before the final C3 atomic refinement.
C3 firmware validation is being repeated after that refinement.
No Git staging, commits, downloads, PIO commands, serial commands, SDK edits, dictionary edits or font edits were performed by this task agent.

## Result and ownership

Home and Recent Books Grid retain the existing ActivityManager render task. A generation published through nonblocking atomic loads/stores invalidates an entire
captured batch. Main-side input sends cancellation before taking a short drain lock, processes the existing input callbacks with
cancellation still sticky, and authorizes only the completed intent. An intervening lifecycle cancellation prevents reauthorization.
A render arriving after cancellation cannot authorize itself by sampling the current generation. Carousel sizes and following grid
items share the captured generation and stop after mismatch. There is no new task, task stack or framebuffer.

The generating activities retain per-visit attempt records, indexed alongside the owning recent-book path, including observed source
CRC/size and requested dimensions (both carousel sizes). A repeated repaint does not retry a failed candidate. Explicit reload or a
later visit clears these records; source replacements are revalidated on that next attempt. This is a deliberately conservative retry
policy: changing a file externally does not itself trigger a background rescan during the same visit. Previous art and Unknown cover
state are retained on transient failure.

`Activity::requestBackgroundCancellation()` is atomic-only and precedes lock acquisition in the shared
`prepareBackgroundSuspension<RenderLock>` gate. Push/Replace/Pop requests also revoke immediately. Existing locked
`prepareToSuspend()` semantics and manga prefetch result consumption are unchanged. Home/Grid report no active render-owned work
once the lock drains. Renderer access and activity lifetime protection in `renderTaskLoop` remain under its original lock.

Sleep uses one lazily started, sticky `SleepCoverBudget` across all manga variants in an entry. The token starts before manga source
validation and includes CRC, decode, temporary validation and publication. `CROSSINK_SLEEP_COVER_GENERATION_BUDGET_MS` defaults to
2,500 ms, with a positive-value check and a firmware check against half `CONFIG_ESP_TASK_WDT_TIMEOUT_S`.
This is an **unmeasured policy**, derived from half the configured five-second watchdog window, not a hard latency guarantee.
Unsigned elapsed subtraction handles millis wrap. A slow read/codec/close can overshoot; the callback records maximum observed poll
spacing. The synchronous sleep path is quiescent before returning. Logs include policy, elapsed time, maximum poll gap, stage,
source type/source dimensions when decoded, requested dimensions, result and whether cached art or no cover is selected.
A cache hit does not decode merely to populate source dimensions; those diagnostic fields may remain zero on that path.

## C3 atomic implementation evidence and writer constraint

The first C3 compile rejected `std::atomic<uint32_t>::is_always_lock_free`: the C3's RV32IMC ISA has no A extension.
Installed ESP-IDF `components/newlib/priv_include/esp_stdatomic.h:35-48,148-172` implements its missing scalar RMW operations
with bounded interrupt-mask/restore sequences on one core; `components/newlib/src/stdatomic.c:25-33` instantiates those helpers.
That investigation establishes why the type-wide lock-free trait is false; the final implementation does not use those RMW helpers.

Cancellation is **single-writer, main-task only**. It now loads the current generation relaxed, increments locally, and stores release.
Cross-task generation/authorization observations use acquire loads. `std::atomic<uint32_t>` remains the storage type, with scalar
size/alignment guards. The exact production `MangaCoverWork.h` compiled using installed Espressif GCC 14.2.0 with
`-march=rv32imc -mabi=ilp32 -std=c++20 -Os` produces `lw`, `addi`, `fence`, `sw` for cancellation and scalar loads/fences for polling;
there are no atomic-helper calls, interrupt-mask calls, locks or retry loops. Evidence lives in
`/private/tmp/crossink-cover10a-owner-atomic-probe.cpp` and `.s`. This probe also caught and fixed a missing explicit `<cstdint>`
include in `MangaCover.h`.

Writer audit: Home/Grid `loadRecentBooks` and `onResume` authorize on main; `MangaCoverInput` sends/authorizes from the main activity
loop; ActivityManager invokes lifecycle cancellation on main. The render task only captures/polls batches and changes its locked
active flag. **Adding a second generation writer would invalidate the load-plus-store increment contract** and requires redesign;
the separate operations are intentionally not advertised as a general-purpose atomic read-modify-write. The 1,000-generation native
regression and the real conversion/suspension-owner tests pass with this final implementation (19/19 RealCoverTest).

## Conversion and publication

`generateThumbnailControlled` returns Cached, Published, Cancelled or Failed; the existing bool wrapper retains its defaults and maps
Cached/Published to true. JPEG/PNG sized converter APIs accept optional borrowed cancellation and dimension-output arguments.
CRC polls after each 256-byte chunk. BMP polls before source/output rows, PNG before decoded scanlines and emitted scaled rows,
and JPEG at MCU callbacks and output rows. Final decoded rows, output sync, identity sync and pre-promotion are covered.
A local latch makes an observed cancellation sticky even if a borrowed callback later changes its answer.

Checked Print forwarding makes every header/row short write fail the conversion. Source/output handles close explicitly on failure,
and temporary BMP/identity files are removed. Cache validation failures caused by failed open/close are errors, not evidence that
old art is disposable. A complete temporary image and synced identity are prepared before touching the previous pair. Existing valid
files move to `.bak` siblings; the brief rename transaction masks cancellation and either promotes both new files or rolls back.
Complete backups and either crossed backup/primary arrangement can restore a missing or invalid primary pair.
Recovery verifies a candidate's exact dimensions and full BMP CRC before mutation. If a recovery rename fails,
the remaining pair is recognized on the next attempt. Cancellation during candidate verification leaves files untouched.

Review fix round 1 bumps the disposable cache to `thumb_v3` and the sidecar to 40-byte MCG3. Its explicit little-endian
field offsets are documented in `docs/file-formats.md`; v2 caches regenerate and no durable user data migrates. New publication
checks exact emitted dimensions against codec-reported source geometry, using the existing adaptive-contain float truncation
and progressive JPEG eighth-scale dimensions. Warm hits bind those exact dimensions and the entire BMP CRC to the source/request
identity. The real 80 × 120 → requested 200 × 390 → emitted 200 × 300 result remains valid. In-bounds 123 × 1 and 1 × 180
malformations, same-extent pixel corruption, and mismatched crossed generations are rejected. Converter pixels, dithering,
scaling and row order remain unchanged. CRC is accidental-corruption/pair identity checking, not adversarial authentication.

## Allocation accounting

All new transaction storage is fallible and local to one synchronous attempt. Cache paths use one `CoverPaths` allocation of 652 bytes
(48+96+100+100+104+100+104); the source path is `folder.size()+258` bytes. These buffers exceed small task stacks and are released on
all returns. The controlled transaction does not construct growing strings for `.tmp` or `.bak` names.

Each dither class has checked `begin(width)` and owns its error rows via unique pointers; raw aliases rotate without reallocating.
Atkinson uses three `(width+4)*2` byte rows (4,824 bytes for an 800-pixel cover); Floyd-Steinberg uses two `(width+2)*2` rows.
Objects and rows publish only after successful allocation. The BMP renderer's existing optional dither callers now handle failure
without replacing its pixel implementation; manga BMP thumbnails still use non-dithered rows.

PNG uses a fallible context allocation instead of placing its 2,048-byte compressed-input buffer, 768-byte palette and InflateStream
state on the render stack. This adds roughly 3 KB of transient heap ownership while removing that large stack frame; it is not a heap
saving. Two source scanlines are each at most 16,384 bytes; the gray row is at most 2,048 bytes; the one-bit output row is at most
100 bytes for an 800-pixel cover. Scaling arrays are `width*4` and `width*2` bytes (4,800 bytes combined at 800 pixels).
All are checked, allocated once, reused through row loops and released automatically. InflateStream retains its existing fallible
state/window allocation, including the 32,768-byte history window. JPEG retains its existing decoder and single arena ownership.
No PSRAM assumption or second full-screen buffer was introduced. Per-visit retry records are bounded activity members.

## Verification and RED/GREEN evidence

Before production edits, a temporary real-codec harness captured 50 PNG/JPEG pixel CRC32/FNV vectors. It also demonstrated that
short PNG writes incorrectly returned true. Repository regressions then failed for both JPEG and PNG short writes, contained-size
publication, and preservation of the old pair after failed promotion. The later close-fault matrix exposed validation-close errors
incorrectly allowing replacement; that regression was fixed and rerun. New cancellation/dither/owner/budget APIs additionally had
explicit missing-API compilation failures before implementation; these are separate from the behavioral RED evidence.

Commands used (offline, serial target builds):

```sh
cmake -S test -B /private/tmp/crossink-cover10a-test \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/private/tmp/crossink-manga-pixel-build/_deps/googletest-src
cmake --build /private/tmp/crossink-cover10a-test --target RealCoverTest MangaCoverTest -j1
/private/tmp/crossink-cover10a-test/manga_cover/RealCoverTest
/private/tmp/crossink-cover10a-test/manga_cover/MangaCoverTest
cmake --build /private/tmp/crossink-cover10a-test --target MangaBitmapPixelsTest -j1
/private/tmp/crossink-cover10a-test/manga_bitmap_pixels/MangaBitmapPixelsTest
git diff --check
```

Results: RealCoverTest **19/19**, MangaCoverTest **9/9**, MangaBitmapPixelsTest **7/7**. The extra bitmap target fix adds its missing
real `lib/Memory` include directory; there was no faithful Memory stub to update, and a host system header had been selected instead.
Golden coverage is 5 real codec fixtures × 10 Home/Grid/sleep dimensions. Tests inject every observed project allocation in each
baseline/progressive JPEG and RGBA/palette/gray PNG attempt, including nothrow objects/rows, real JPEG arena and real inflater state/window.
Storage faults cover indexed header/row/identity short writes, both syncs, all observed closes and all four backup/promotion renames.
Every failed attempt checks old pair bytes, temporary cleanup and zero open handles. Cancellation sweeps every observed codec and full
cover poll, including BMP final output, CRC and immediately before promotion. Twelve real conversion threads block during CRC with an
open source, receive cancellation before the shared suspension gate acquires its mutex, and drain before transition readiness.
Separate tests cover stale queued batch rejection, complete backup recovery, corrupt/oversized cached dimensions, wrap-safe sleep expiry
and CRC expiry suppressing a later sleep variant.

The native owner tests execute the same cancellation owner and suspension gate used by production, with real cover conversion and
host mutex/storage boundaries. They do not instantiate the entire Home/Grid UI or a physical FreeRTOS scheduler. Full app input/simulator
and firmware validation belong to the independent root gate. Simulator codec stubs cannot prove image timing or pixel correctness.
Compiler output retains pre-existing unused codec-option warnings; googletest emits its host char8_t conversion warning.
Logs: `/private/tmp/crossink-cover10a-build.log`, `-matrix.log`, `-legacy.log`, `-bitmap-build.log`, `-bitmap-test.log`; final atomic refinement: `-atomic-build.log`, `-atomic-tests.log`.

## Hardware verification still required

On X3/X4 C3 and Sticky/X4 Pro S3, use large valid baseline/progressive JPEG and PNG first pages. Delete only the relevant
`/.crosspoint/manga_<hash>/thumb_v3_*` disposable cover files for an uncached run, retaining other reading data. Exercise rapid Home
carousel movement and Grid paging during generation, cancel near final rows, then enter sleep and immediately enter file transfer
or X4 Pro USB Drive. Expected: old art/placeholder remains on cancellation or failure, the selected page/book stays current, no partial
BMP/identity pair is consumed, and no open-handle/storage ownership error appears. Repeat cancellation/re-entry while logging free and
largest internal heap (plus S3 PSRAM), task stack watermarks, policy elapsed/max-poll-gap diagnostics and total sleep-entry duration.
Check cached and uncached minimal, dashboard and full-cover modes. Record distributions before changing the 2,500-ms policy; retain the
cooperative overshoot caveat. No physical latency, watchdog margin or long-run heap stability is certified by host tests/builds.

## Exact changed existing files against the Task 10a snapshot

- `CHANGELOG.md`
- `lib/GfxRenderer/Bitmap.cpp`
- `lib/GfxRenderer/Bitmap.h`
- `lib/GfxRenderer/BitmapHelpers.h`
- `lib/JpegToBmpConverter/JpegToBmpConverter.cpp`
- `lib/JpegToBmpConverter/JpegToBmpConverter.h`
- `lib/MangaPanel/MangaCover.cpp`
- `lib/MangaPanel/MangaCover.h`
- `lib/PngToBmpConverter/PngToBmpConverter.cpp`
- `lib/PngToBmpConverter/PngToBmpConverter.h`
- `src/activities/Activity.h`
- `src/activities/ActivityManager.cpp`
- `src/activities/boot_sleep/SleepActivity.cpp`
- `src/activities/boot_sleep/SleepActivity.h`
- `src/activities/boot_sleep/SleepCoverAssets.cpp`
- `src/activities/boot_sleep/SleepCoverAssets.h`
- `src/activities/home/HomeActivity.cpp`
- `src/activities/home/HomeActivity.h`
- `src/activities/home/RecentBooksGridActivity.cpp`
- `src/activities/home/RecentBooksGridActivity.h`
- `test/manga_bitmap_pixels/CMakeLists.txt`
- `test/manga_cover/CMakeLists.txt`
- `test/manga_cover/stubs/Converters.cpp`
- `test/manga_cover/stubs/JpegToBmpConverter.h`
- `test/manga_cover/stubs/PngToBmpConverter.h`

## New production files

- `lib/CooperativeCancellation/CheckedPrint.h`
- `lib/GfxRenderer/BmpConversionDimensions.h`
- `src/activities/BackgroundSuspension.h`
- `src/activities/home/MangaCoverWork.h`
- `src/activities/home/MangaCoverInput.h`
- `src/activities/boot_sleep/SleepCoverBudget.h`

## New test/fixture files

- `test/manga_cover/RealCoverTest.cpp`
- `test/manga_cover/fixtures/README.md`
- `test/manga_cover/fixtures/baseline.jpg`
- `test/manga_cover/fixtures/goldens.txt`
- `test/manga_cover/fixtures/gray.png`
- `test/manga_cover/fixtures/mono.bmp`
- `test/manga_cover/fixtures/palette.png`
- `test/manga_cover/fixtures/progressive.jpg`
- `test/manga_cover/fixtures/rgba.png`
- `test/manga_cover/real_stubs/Arduino.h`
- `test/manga_cover/real_stubs/Arena.h`
- `test/manga_cover/real_stubs/FaultAllocation.h`
- `test/manga_cover/real_stubs/HalDisplay.h`
- `test/manga_cover/real_stubs/HalStorage.cpp`
- `test/manga_cover/real_stubs/HalStorage.h`
- `test/manga_cover/real_stubs/InflateStreamForTest.cpp`
- `test/manga_cover/real_stubs/Logging.h`
- `test/manga_cover/real_stubs/Print.h`
- `test/manga_cover/real_stubs/freertos/FreeRTOS.h`
- `test/manga_cover/real_stubs/freertos/task.h`

## New report and snapshot additions

- `docs/superpowers/plans/2026-09-07-manga-cover-completion-report.md`
- Extra pre-edit snapshots: `lib/GfxRenderer/Bitmap.cpp`, `lib/GfxRenderer/Bitmap.h` (when absent in the original set),
  and `test/manga_bitmap_pixels/CMakeLists.txt`. `lib/Memory/BuildScratch.cpp` was also copied for preflight but remains unchanged.
- Baseline snapshot: `/private/tmp/crossink-manga-cover10a-before`; baseline harness/CRCs: `/private/tmp/crossink-cover10a-baseline`.


## Review fix round 1 — 2026-09-07

The two High findings and Low diagnostic finding in the independent review are addressed. The source/test handoff is frozen
for root's next firmware builds and independent review; earlier firmware sizes and smoke evidence are pre-fix only.

Before edits, all 14 touched files were copied to `/private/tmp/crossink-manga-cover10a-fix1-before`; its `manifest.txt` lists:

- `docs/file-formats.md`
- `docs/manga-covers.md`
- `docs/superpowers/plans/2026-09-07-manga-cover-completion-report.md`
- `lib/MangaPanel/MangaCover.cpp`
- `lib/MangaPanel/MangaCover.h`
- `lib/GfxRenderer/BmpConversionDimensions.h`
- `lib/JpegToBmpConverter/JpegToBmpConverter.cpp`
- `src/activities/boot_sleep/SleepCoverAssets.cpp`
- `src/activities/boot_sleep/SleepActivity.cpp`
- `test/manga_cover/RealCoverTest.cpp`
- `test/manga_cover/real_stubs/HalStorage.h`
- `test/manga_cover/real_stubs/HalStorage.cpp`
- `test/manga_cover/stubs/Converters.cpp`
- `test/manga_cover/MangaCoverTest.cpp`

No new production/test files were added in this round. Existing changelog coverage remains applicable. The format documentation
was updated before the implementation changed to MCG3. Compile-time checks enforce 40-byte size, relevant offsets and the
supported little-endian representation. There is no journal or new header parser. Geometry comes from the real converters;
JPEG now reports its progressive flag alongside its physical dimensions. All 50 pre-change vectors also pass through real
publication and warm reuse with exact golden pixel CRCs and sidecar field checks.

Recovery considers valid primary, complete backup, then either crossed arrangement. A full BMP digest binds each candidate
before any removal/rename; mismatched candidates remain untouched. A failed second recovery rename leaves a recognized crossed
pair for the next attempt. Source CRC, generated BMP CRC, warm-hit CRC and recovery CRC share the existing 256-byte cooperative
stack buffer; no new heap allocation was added. Identity grows from 32 to 40 bytes, source-dimension records from 8 to 12 bytes,
and recovery uses six borrowed pointers (24 bytes on ESP32) plus a scalar index. These are bounded local records; the existing
652-byte fallible path owner and source-path allocation lifetimes are unchanged. Additional checksum reads cost SD time;
no latency or throughput improvement is claimed. The 2,500 ms sleep budget remains an unmeasured cooperative policy.

Diagnostics now preserve `Cached`, `Published`, `Cancelled`, or `Failed` through the bool-compatible SleepCoverAssets API.
Early sleep preparation cancellation sets `Cancelled`; other preflight failures retain `Failed`. Sleep logs print the named
result and `cache_hit` separately from the existing fallback choice. The controlled API resets diagnostics for each attempt
and sets the result on every return.

TDD / regression evidence:

- `/private/tmp/crossink-cover10a-fix1-red.log`: both High regressions fail before production changes. Structurally complete
  123×1 and 1×180 BMPs were incorrectly Cached, and persistent rename failure beginning at the second backup move stranded
  the old BMP after reopening and cancelling before new conversion. Both now pass.
- `/private/tmp/crossink-cover10a-fix1-diagnostic-red.log`: the result-propagation regression fails for Published, Cached and
  Cancelled with the result assignment temporarily disabled; restoring the assignment passes. This is a mutation check.
- Added mismatch tests for both crossed directions using different real PNG pixel generations; every recovery CRC boundary
  cancellation; persistent failure of either backup-recovery rename; same-extent pixel corruption; and 50 golden publication,
  layout and warm-reuse vectors. Existing allocation/finalization/cancellation matrices and actual render-owner drain remain green.

Commands, run serially after the final source edit:

```sh
cmake --build /private/tmp/crossink-cover10a-test --target RealCoverTest MangaCoverTest -j1
/private/tmp/crossink-cover10a-test/manga_cover/RealCoverTest
/private/tmp/crossink-cover10a-test/manga_cover/MangaCoverTest
git diff --check
```

Final output: **27/27 RealCoverTest**, **9/9 MangaCoverTest**, zero failed tests; `git diff --check` clean.
Logs: `/private/tmp/crossink-cover10a-fix1-green-build.log`, `/private/tmp/crossink-cover10a-fix1-green.log`,
`/private/tmp/crossink-cover10a-fix1-legacy.log`. RealCoverTest includes the unchanged 50-vector codec golden test plus the new
50-vector publication/reuse test. The known unused codec-toggle warnings occurred on the clean codec rebuild; no new warning
was introduced. Root owns broad native, PIO, simulator smoke and independent review. No PIO, serial, commits or subagents were used.

Hardware follow-up: on C3 and each S3 storage path, clear only `thumb_v3_*` for a cold run, verify MCG3 sidecars and correctly
proportioned 200×300 contained art, then repeat warm Home/Grid/sleep visits. Capture the named result/cache_hit/fallback fields,
heap/maxAlloc and poll gaps. Exercise navigation/sleep/transfer cancellation while decoding. With a fault-enabled development
storage adapter, fail the second backup rename and subsequent rollback, then resume/reopen and verify the old pair becomes
readable before a cancelled fresh attempt. This persistent-fault case is host-tested; physical SD fault behavior and timing
remain unmeasured. Old v2 artifacts may be removed as disposable cache; keep `stats_v5.bin`, reading state and book files.


### Fix round 1 smoke-runner integration

Root's C++ manga stress and touch/OCR smoke flows completed, but the Python artifact validator still expected `thumb_v2`
basenames and exited 2. Updated `scripts/run_manga_simulator_smoke_test.py` to require the v3 Recent Books and Home names.
Existing BMP, pixel-cache and statistics checks remain; the cover check also requires a 40-byte MCG3 sidecar with matching
requested/emitted dimensions and full BMP CRC. This script is the fifteenth file in the fix-round snapshot manifest and was
copied before editing into both `/private/tmp/crossink-manga-cover10a-fix1-before` and the original Task 10a snapshot.
No firmware/native source changed. Python AST compilation and `git diff --check` pass; root owns rerunning both real smokes.
The runner is frozen with the firmware/test inputs pending those integration results.

`python3 /private/tmp/crossink-cover10a-runner-check.py` additionally executed the actual `validate_library` function against
a temporary v3 BMP/identity/Home/statistics fixture: valid artifacts passed and a corrupted MCG3 digest was rejected.
