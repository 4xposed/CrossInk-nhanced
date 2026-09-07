# Task 8b — one-slot cancellable manga warming

This report covers the worker integration, not hardware acceptance. No commits,
pushes, SDK changes, font changes, or PlatformIO invocations were made by the
worker agent. The parent owns serial firmware builds and simulator runs.

## Source ownership and behavior

- `lib/MangaPanel/MangaPrefetchState.h`: a single request generation and atomic
  Idle/Posted/Running/Finished handshake. The owner cannot overwrite a request
  or reuse source/cache ownership until it consumes the producer's release
  acknowledgment. Cancelling invalidates the generation without releasing the
  slot. The worker closes all files and destroys decoder/BMP scratch owners
  before publishing Finished.
- `src/activities/reader/MangaPrefetch.{h,cpp}` and the two `MangaPrefetch*Budget.cpp`
  files: one fallibly created priority-zero FreeRTOS task, copied bounded source
  and folder paths, one geometry/config snapshot and one scalar result. It uses
  independent converter instances and never accesses a renderer, RenderLock,
  MangaBook/PageView, or activity API. A valid existing payload is fully opened
  and validated before being treated as a hit. Cancellation is checked before
  publication; incomplete temporary files are removed before acknowledgment.
- `lib/MangaPanel/MangaImageGeometry.{h,cpp}` and `applyImageLayout()` in
  `MangaPrefetch.cpp`: the foreground and worker share exact geometry and identity
  policy. The reader captures both base and rotated bezel-safe viewports under
  RenderLock, including asymmetric insets. BMP retains no-upscaling/no-dither
  behavior; JPEG/PNG retain upscaling and dithering. Orientation and output
  screen extents are explicit in cache identity.
- `MangaReaderActivity.{h,cpp}`: prepare candidate paths only while idle using
  the foreground adapter, preserving canonical/legacy resolution without
  invalidating its borrowed PageView. Warm the first/next available crop after
  150 ms, then the next overview after 400 ms. Resource failures back off for
  30 seconds; deliberate cancellation does not. Monochrome BMP is skipped.
  Navigation, menu, rotation, Back-to-overview and redraw intent revoke warming
  before foreground file work. While draining, input keeps being polled; moves
  coalesce into a bounded signed count and rotations modulo four. Deferred
  renders are requested again after cleanup. Progress observation that can write
  is skipped while the worker owns a request.
- `Activity.h:40`, `ActivityManager.cpp:225`: a default no-op readiness hook
  preserves pending Push/Replace/Pop until the outgoing activity drains.
  Pending transitions skip another outgoing activity loop, preventing reposts.
  Child Pop invokes the new no-op-by-default onResume hook so manga may warm
  again after its next render. The manga exit destructor defensively cancels
  and joins before progress/stats/bookmark saves or ownership destruction; no
  forced task deletion or timeout frees live context.
- `main.cpp:908`: sleep latches and retries readiness before APP_STATE writes,
  screen snapshots, or sleep-screen transition. GPIO polling continues during
  draining. This closes the pre-transition persistence path that an activity
  manager hook alone would miss.
- `src/simulator/SimulatorSmokeTest.cpp`: normal manga smoke now dwells while
  leaving the main loop active. Optional
  `CROSSINK_SIMULATOR_MANGA_PREFETCH_STRESS=1` adds a deterministic barrier at the first fingerprint checksum poll
  with a source file open, released only by cancellation, and actual child push/pop, reader pop,
  replacement, manual refresh and main sleep integration assertions. It is
  simulator-only; normal firmware has no test barrier. A stress-only interception
  before the simulator HAL sleep call validates completed sleep preparation
  without entering the simulator's intentional wait-for-wake loop.
- `test/manga_prefetch/`: state/geometry tests plus the actual worker compiled
  with native task/HAL doubles and real locally installed JPEGDEC/PNGdec.
  `test/CMakeLists.txt` registers these targets. CHANGELOG records idle warming.

## Narrow Task 8a path adapters required for fallible setup

The original std::string-only cache APIs would make reserve/copy allocations
abort under -fno-exceptions even inside makeUniqueNoThrow. Preflight heap checks
cannot change that allocation contract. Worker setup therefore uses only
fallible char arrays (source capacity capped at 1024 bytes; folder length must
fit); failure disables speculation and leaves foreground reading available.

Existing std::string decoder entry points remain wrappers. New const-char cache
entry points and `getDimensionsForCache()` avoid implicit source-path copies.
`RenderConfig::cachePathOverride` is an optional borrowed output path, retained
through synchronous decode; the existing owning cachePath remains supported.
`PixelCache::begin(const char*)` borrows through finalize/abort while its string
entry preserves the existing ownership contract. `MangaPixelCache::configure`
also accepts a const-char folder. JPEG/PNG open callbacks call the existing HAL
const-char overload directly. PNG's unsupported-bit-depth log avoids allocating
concatenated strings. These changes do not alter decoding/scaling or cache format.

Files touched for these adapters: `ImageToFramebufferDecoder.h`, JPEG/PNG
converter headers and implementation, `PixelCache.h`, and `MangaPixelCache.{h,cpp}`.
The pre-8b snapshot is `/private/tmp/crossink-prefetch-worker-before`.

## Actual memory budget and limits

No framebuffer or implicit PSRAM allocation was added. Persistent cost is one
8192-byte task stack, a bounded worker/config/cache context, and two fallible path
buffers totaling at most 2048 bytes. The 9216-byte BMP row scratch is now job-scoped,
allocated once per speculative BMP (never per row), and freed before completion.
This avoids charging that allocation to every later foreground PNG. JPEG/PNG
objects and cache bands are likewise job-scoped within the existing decoder.

C3 decoder object sizes were verified from the **actual default-build object
code**, not estimates or host sizes:

```
riscv32-esp-elf-objdump -d \
  .pio/build/default/src/activities/reader/MangaPrefetchJpegBudget.cpp.o \
  .pio/build/default/src/activities/reader/MangaPrefetchPngBudget.cpp.o
```

The functions return JPEG `0x45dc = 17884` and PNG `0xe840 = 59456` bytes.
PNG includes `PNG_MAX_BUFFERED_PIXELS=16416`. The budget additionally includes
25088 bytes for the maximum 24-KiB band plus packed spare row, 2048 bytes for a PNG
gray row, 4096 bytes for small file/path/allocator overhead, and a **32768-byte
internal-RAM reserve**. Stack and persistent context have already been deducted
from observed free memory at job admission. Both total free and largest free
block use `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` on hardware.

| Candidate | Required current internal free | Required largest block |
| --- | ---: | ---: |
| BMP | 46080 | 13312 |
| JPEG | 79836 | 29184 |
| PNG | 123456 | 63552 |

The largest-block guard adds 4096 bytes to the largest individual allocation.
Task creation separately admits stack plus overhead with the same reserve.
This is conservative admission, not a measured working-set or latency guarantee.
The durable 85–90-KiB C3 reading baseline is **before** this worker's persistent
cost: JPEG warming may be marginal and PNG warming is intentionally rejected
at that baseline. BMP can still warm. The persistent 8-KiB task remains a cost
for foreground decodes; measure real C3 scenarios before tuning the reserve.
Host budget log sizes (JPEG17944/PNG59528) are not substituted for C3 sizes.

## Verification

Offline native commands:

```
cmake -S test -B /private/tmp/crossink-prefetch-tests \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/private/tmp/crossink-manga-pixel-build/_deps/googletest-src
cmake --build /private/tmp/crossink-prefetch-tests \
  --target MangaPrefetchTest MangaPrefetchWorkerTest ImageCacheDecodeTest MangaPixelCacheTest -j2
ctest --test-dir /private/tmp/crossink-prefetch-tests \
  -R 'MangaPrefetch|RealCodecs|PixelCacheTest' --output-on-failure
```

**43/43 passed**, sequential CTest. Logs:
`/private/tmp/prefetch-worker-native-build.log` and
`/private/tmp/prefetch-worker-native-tests.log`. Tests include copied path lifetime,
one-slot rejection, stale generation, cancellation during final sync, temporary
cleanup, successful next generation, bounded-path/task-creation failures,
shutdown-before-owner-release, actual JPEG/PNG renderer-free production, and
byte-identical BMP foreground/prefetched payloads. Existing real-codec foreground
parity/golden hashes and all manga cache validation tests pass after the adapters.

The initial state/geometry target failed before implementation because the new
state header was absent, then passed. Native worker fixture failures were corrected
by using the HAL double's documented absolute source paths; they were fixture
setup errors, not decoder regressions. `git diff --check` passes.

Parent-reported runs before the last simulator stress addition:

- `pio run -e simulator`: passed.
- `pio run -e default`: passed, image 6430176 bytes, 123424 bytes free.
- Monochrome manga simulator smoke: passed.
- Grayscale manga smoke: passed; logs show background creation of page 0 panel 0
  and page 1 overview before entry, followed by foreground cache hits and matching
  BW/LSB/MSB/restored-BW hashes.
- Existing EPUB/dictionary simulator smoke: passed.

Parent logs are `/private/tmp/crossink-manga-prefetch-worker-{simulator,default,smoke,gray-smoke,epub-smoke}.log`.
The full offline native build and sequential CTest run passed **634/634**:
`/private/tmp/prefetch-worker-all-build.log` and
`/private/tmp/prefetch-worker-all-tests.log`. The first simulator stress run timed out at 90 seconds. Captured reproduction
showed all its preceding transitions completed and main entered deep sleep at
3776 ms; simulator `HalGPIO::startDeepSleep()` intentionally waits for a wake
event, so reopening a reader afterward cannot proceed. That harness defect was
fixed with the stress-only final-HAL interception described above. The trace
also showed a fixed dwell could encounter an already-finished cache hit, so the
stress harness now waits for an observable open-source barrier. Its script
preserves captured output on TimeoutExpired rather than losing failure evidence.
The updated stress result remains pending.
Final stress results, builds/review and any S3 build remain to be recorded; do
not infer those results here.

## Hardware acceptance still required

On X4, compare cold and warmed BMP/JPEG/PNG pages and panels after clearing only
the relevant disposable `/.crosspoint/manga_<folder-crc>/` pixel cache. Verify the
same framing/dither/gray levels. Pause for warming, then rapidly move both ways,
rotate repeatedly, manually refresh, open child menus/global settings/frontlight,
return, exit, and sleep during large JPEG/PNG work. Expected behavior: responsive
input polling, cancellation followed by foreground cache/reader ownership, no
partial published cache or duplicate-reader errors, and normal progress/stat saves.
Record the new bounded worker shutdown log (internal free/largest heap and
uxTaskGetStackHighWaterMark) over repeated cycles. Repeat on an S3 profile. Host HAL permits more concurrent access than the
physical SD implementation, so host results cannot prove physical SD exclusion,
watchdog margin, latency, or the practical C3 codec admission rate.


## Review fix round 1

The independent review found that a fresh direction on the completion iteration
could execute before an old retained direction, producing a nonzero turn at a
boundary even when the two directions should cancel. A successful fresh menu
open also left the old menu flag behind. The deterministic lifecycle stress
prefix passed, but its later chapter-selection step failed: the next Confirm
press arrived on the iteration when the deferred menu opened at the loop's end,
after the menu input handler had already been skipped.

`MangaPendingInput.h` now collects and consumes bounded directional, menu and
rotation intent. Every direction first joins the retained net movement; only
`moveLocked()` in the single drain path applies it. Menu requests coalesce and
are consumed before `OptionPopup::handleInput()` sees that completion frame's
input, so a new Confirm press selects the menu instead of being lost. An already
active menu absorbs another menu-open request. Rotations compose before one
settings save rather than applying a fresh rotation before older ones.

Three native controller regressions cover opposing directions at both bounds,
exactly-once menu consumption, and composed rotations. Simulator stress now uses
a separate consumption gate to hold an acknowledged Finished request until a
fresh edge is injected, and reads the actual manga reader's position/menu state:
first overview stays at 0/-1 after retained Forward plus fresh Back; final panel
stays at 1/0 after retained Back plus fresh Forward; duplicate menu intent plus a
Confirm press on the drain frame opens chapter selection and returns without
reopening the menu. The last-boundary test explicitly posts the current source
through the normal worker because there is naturally no upcoming candidate at
the last panel. All these accessors and scheduling controls are simulator-only.

Commands after the fix:

```
cmake --build /private/tmp/crossink-prefetch-tests   --target MangaPrefetchTest MangaPrefetchWorkerTest -j2
ctest --test-dir /private/tmp/crossink-prefetch-tests   -R 'MangaPrefetch|MangaPendingInput' --output-on-failure
```

Result: **14/14 passed**; build log
`/private/tmp/prefetch-fix1-native-build.log`. A full rebuild and sequential CTest
also passed **637/637** in 30.98 seconds (`/private/tmp/prefetch-fix1-all-build.log`
and `/private/tmp/prefetch-fix1-all-tests.log`). The parent simulator build passed
(`/private/tmp/crossink-manga-prefetch-fix1-simulator.log`) and the **full
deterministic grayscale stress passed** at 32.096 seconds
(`/private/tmp/crossink-manga-prefetch-fix1-stress.log`), including persisted-state
and grayscale-plane hash validation. The log explicitly verifies 0/-1 and 1/0
net-zero boundaries, menu-drain Confirm selection, no duplicate menu reopening,
held-source transition deferrals, and deferred/completed main sleep preparation.

Independent scoped re-review marks the P2 **ADDRESSED**, with both scoped spec
and quality verdicts **pass** and no new blocking issue. See
`2026-09-06-manga-prefetch-worker-review.md`. Final C3 build, any S3 checks,
completion ledger and hardware acceptance remain owned by the parent. No further
production changes are pending from this worker task.
