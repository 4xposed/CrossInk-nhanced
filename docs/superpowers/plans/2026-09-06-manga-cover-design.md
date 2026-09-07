# Manga cover cancellation and allocation design

## Scope and evidence

Manga thumbnail generation currently checksums the full first-page source and converts it synchronously before publishing
the BMP and identity sidecar (`lib/MangaPanel/MangaCover.cpp:159-236`). Home invokes this from its render path for both
carousel sizes and every other theme size (`src/activities/home/HomeActivity.cpp:667-855`, called from render at
`:2111`, `:2165`, and `:2232`). Recent Books Grid does the same for the visible page
(`src/activities/home/RecentBooksGridActivity.cpp:254-335`, called from render at `:830`). The list-style
`RecentBooksActivity` consumes existing cover paths but does not generate manga thumbnails. Sleep cover rendering calls
the same generator synchronously from `SleepActivity::onEnter()` through `SleepCoverAssets`
(`src/activities/boot_sleep/SleepCoverAssets.cpp:50-94`; `src/activities/boot_sleep/SleepActivity.cpp:494-554`,
`:714-752`, `:784-853`).

The reusable cancellation type already exists as `CooperativeCancellation`, a borrowed callback/context polled at bounded
work boundaries (`lib/CooperativeCancellation/CooperativeCancellation.h:3-9`). It is already used by manga pixel cache
and BMP paths, so cover work should use this contract rather than introduce another token type.

PNG one-bit cover generation reaches unchecked allocations at `lib/PngToBmpConverter/PngToBmpConverter.cpp:697-714`.
Wrapping the outer ditherer with nothrow allocation is insufficient because all three ditherer classes allocate their own
error rows with bare `new[]` (`lib/GfxRenderer/BitmapHelpers.h:24-35`, `:105-117`, `:206-215`). JPEG already uses a
fallible arena for its row/scaling buffers, but its nothrow ditherer construction has the same hidden constructor risk
(`lib/JpegToBmpConverter/JpegToBmpConverter.cpp:684-756`). BMP thumbnail conversion already uses one checked row scratch
allocation (`lib/MangaPanel/MangaCover.cpp:90-130`) and should retain its current non-dithered behavior.

## Chosen execution model

Use the existing ActivityManager render task for Home and Grid generation. These functions already run there, the renderer
and their activity state already remain under `RenderLock`, and the main task continues to receive input while conversion
runs. Adding another worker would add a second job queue, another stack, cross-task access to recent-book state, and new SD
ownership without making conversion more cancellable.

Each generating activity owns a monotonically increasing atomic cancellation generation. A render batch captures the
current generation once before its first candidate; every center/side or visible-grid item in that batch carries the same
captured value. Cancellation increments the atomic and is never cleared between thumbnails. The borrowed cancellation
callback rejects any mismatch, and render checks the mismatch again before opening the next source or publishing a result.
Only a later, explicit activity intent that is still relevant after input/state processing may capture the new generation
for a new batch. This prevents a render task that was waiting for `RenderLock` from clearing an already-delivered input or
lifecycle cancellation and starting the next item. The conversion never calls the renderer: loading
indicators and placeholders are drawn before generation begins, and completion merely updates activity-owned state while
the render still holds its normal lock. There is no second framebuffer.

Home remains sequential and bounded: one `generateThumbnail` call is in flight at a time, and carousel center and side
sizes are separate operations under one captured batch generation. Cancellation after the center size prevents starting
the side size and remains sticky for the rest of that render batch. Grid likewise stops
after the current item. A cancelled or failed item keeps the previous validated cover or placeholder and remains
`CoverState::Unknown`. Record an attempted `(book path, source identity, width, height)` for the current activity visit so
render invalidations do not immediately retry the same failure. A new source identity, explicit reload, or a later
activity entry may retry.

Input that makes the pending result irrelevant sets the atomic before changing selection or visible page: Home carousel
movement/theme reload, Grid page movement, opening a book, Back/Home, and book actions. Normal repaint requests that retain
the same selection do not cancel.

## Suspension and storage ownership

Add a separate `Activity::requestBackgroundCancellation()` virtual hook. It is idempotent, runs on the main task without
`RenderLock`, and may only set atomic cancellation/generation state; it may not touch renderer state, mutate the activity
stack, or access non-atomic worker state. Keep the existing `prepareToSuspend()` contract unchanged: ActivityManager calls
it with `RenderLock` to poll or consume owner results and decide whether every file owner has drained. Returning `false`
keeps the transition pending and suppresses further normal activity input; returning `true` guarantees the activity owns
no open source/output file and no temporary publication operation is executing.

This split matches the actual scheduler. Home and Grid conversion already runs on the render task while holding
`RenderLock`, while the main task continues processing input. As soon as Push/Replace/Pop becomes pending,
ActivityManager calls `requestBackgroundCancellation()` before its existing `prepareToSuspend()` attempts the lock
(`src/activities/ActivityManager.cpp:224-240`, `:388-391`). The atomic request therefore reaches codec callbacks even
when the render task still owns the lock; once conversion closes its files and releases the lock, the reviewed locked
readiness hook can consume completion safely. Deep-sleep preparation invokes the cancellation hook before the same locked
readiness gate. This preserves existing manga prefetch owner/consume logic and avoids changing every
`prepareToSuspend()` override to a new concurrency model.

Home and Grid `requestBackgroundCancellation()` advance the cancellation generation. Their locked `prepareToSuspend()` returns
false while render-owned conversion is active; after the callback closes its source/output handles and removes temporary
files, the next locked poll returns true. Input paths that change selection or visible page set the same atomic before any
activity-side lock acquisition. `onExit()` asserts or logs that no job remains, then clears activity-owned retry state.
This guarantees all file handles close before WebDAV, Nearby, USB mass storage, and sleep transitions.

Sleep does not start another worker. `SleepActivity::onEnter()` currently renders synchronously on the main task, after the
outgoing activity has drained. Its manga cover call receives a cancellation context with a monotonic deadline established
before source identity validation begins, so the full-source CRC consumes the same budget as decode and publication. When
the callback first observes expiry, generation closes and removes the temporary output and reports cancellation; the
existing cached cover or normal no-cover screen is used. When `onEnter()` returns, generation is necessarily quiescent, so
deep sleep never begins with a worker or SD handle alive. Reading a valid cached thumbnail remains unchanged.

No enumerated hardware is currently available, so this task must not describe the initial deadline as measured. Use an
explicit policy default of **2,500 ms for the entire uncached sleep-cover attempt**, starting before MangaBook/source
validation and CRC. The repository configures the ESP task watchdog to 5 seconds
(`sdkconfig.defaults:2369-2374`); taking half that window leaves the other half for the longest cooperative-poll overshoot,
fallback drawing and the remaining sleep-entry work. This is a responsiveness policy and safety margin, not evidence that
all valid covers finish within 2.5 seconds and not a watchdog substitute. `SleepActivity::onEnter()` runs synchronously
before `ActivityManager::goToSleep()` returns (`SleepActivity.cpp:494-554`; `ActivityManager.cpp:570-575`), and main does
not call `startDeepSleep()` until the sleep activity, framebuffer handling, stats backup and device shutdown complete
(`src/main.cpp:908-963`), so an unbounded miss directly delays sleep.

Define the value in the app sleep-cover policy boundary, for example
`CROSSINK_SLEEP_COVER_GENERATION_BUDGET_MS`, defaulting to 2500. Where
`CONFIG_ESP_TASK_WDT_TIMEOUT_S` is available, add a compile-time check that the default/override is positive and no more
than half of that configured window; simulator builds use the explicit 2500-ms default. A board override may lower it for
product policy. Raising it above half the watchdog window requires a separately reviewed watchdog/yield analysis rather
than a local cover tweak. Do not derive it from `allowSleepAt = millis() + 2000` (`src/main.cpp:1424`): that is a wake-time
guard against immediately sleeping again, not an entry-latency budget.

The deadline is cooperative, not a hard latency guarantee: overshoot is bounded by the longest interval between
cancellation polls in CRC, codec callbacks, row emission, close/sync and finalization. On expiry, cancellation becomes
sticky for the whole `SleepActivity` cover batch; fallback rendering must not clear it and start a minimal/dashboard/full
variant afterward. Use wrap-safe elapsed-time subtraction or a deadline helper with defined wrap behavior. Log the policy
budget, total elapsed time, maximum observed time between polls, stage where expiry was observed, source type/dimensions,
cache hit/miss and fallback selected. These are diagnostic observations. Once C3 and S3 hardware are available, record
distributions for CRC, codec, publication and total sleep entry, then revise the policy only through this documented
constant; label those numbers measured and retain the cooperative-overshoot caveat. Tests cover expiry during CRC as well
as immediately before, at, and after each later poll boundary.

## Cancellation propagation and publication

Add an optional `CooperativeCancellation` parameter, defaulting empty for existing EPUB/XTC/sleep callers, through:

- `manga::generateThumbnail`, source CRC, and BMP conversion;
- `JpegToBmpConverter::{jpegFileToBmpStreamWithSize,jpegFileTo1BitBmpStreamWithSize}` and the internal decoder context;
- `PngToBmpConverter::{pngFileToBmpStreamWithSize,pngFileTo1BitBmpStreamWithSize}` and its decode context.

Poll after every 256-byte CRC chunk, before reopening the source, once per BMP source/output row, once per PNG decoded
scanline and each emitted scaled row, and once per JPEG MCU callback/output row. Also poll after the final decoded row and
immediately before `sync`, identity creation, and each publication rename. Polling per pixel is unnecessary overhead.

Cancellability needs a distinct result from conversion failure. Add `generateThumbnailControlled(...)` returning
`ThumbnailResult::{Cached, Published, Cancelled, Failed}` and taking `CooperativeCancellation`; retain the current
`generateThumbnail(...) -> bool` as a source-compatible wrapper using empty cancellation and mapping `Cached`/`Published`
to true. Home, Grid, and sleep use the controlled API. `Cancelled` and `Failed` both close source and output explicitly
and remove both temporary files. Neither removes or rewrites a previously valid final BMP/identity pair.

Publication remains foreground-only and occurs only after the complete temporary BMP validates and the identity temporary
file has synced. Move an existing valid pair to `.bak` siblings before promotion, mask cancellation during the short rename
sequence, and restore both backups if either promotion fails. Remove backups only after both new names exist. Startup/cache
validation may restore a complete backup when the primary pair is absent, and rejects/removes every mixed or partial pair.
This mirrors the repository's durable-state promotion pattern while keeping the cover itself disposable. A cancellation
observed before promotion returns `Cancelled`; one observed after promotion begins finishes promotion or rollback before
reporting quiescence.

## Fallible allocation changes

Make the ditherer objects validatable rather than allocating in constructors that cannot report failure. Each ditherer
gets a fallible `begin(width)` (or a static `create(width)`) that allocates all error rows with
`makeUniqueNoThrow<int16_t[]>`, publishes them to the object only after every allocation succeeds, and exposes no usable
partially initialized state. Store rows as `std::unique_ptr<int16_t[]>`; rotation swaps raw aliases or unique owners without
allocating. JPEG and PNG check creation and log the requested row bytes before decoding.

In PNG conversion, replace bare `new` for the outer ditherers, `rowAccum`, and `rowCount` with checked fallible ownership.
Keep the existing checked `malloc` buffers or convert them mechanically to the existing automatic heap-buffer owner; every
return path must free the inflate window, scanlines, output row, gray row, scaling arrays, and dither rows. Check BMP header
writes and every row write: the current PNG path writes at `PngToBmpConverter.cpp:786` and `:861` without checking the
returned byte count. A short write is `Failed`, never `Published`.

Do not change pixels, thresholds, row order, adaptive contain geometry, or the non-dithered BMP cover path. The allocation
work changes only failure behavior from abort/undefined use to logged cleanup and a retained placeholder.

## Verification

Add host tests that compile the real JPEG and PNG ToBmp converter implementations and their bundled codec dependencies,
instead of the converter stubs currently used by `test/manga_cover`. Use small checked-in baseline JPEG, progressive JPEG,
RGBA PNG, palette PNG, grayscale PNG, and BMP fixtures. Validate decoded output dimensions, complete BMP length/header,
and a golden CRC of pixel bytes for every Home carousel, normal Home, Grid, minimal sleep, dashboard sleep, and full-screen
sleep size. This detects pixel changes while keeping fixtures small.

Provide deterministic allocation injection at the project allocation boundary, then fail each reachable allocation in
turn: decoder object, PNG scanlines, inflate window, output/gray rows, each dither row, scaling accumulators, and JPEG arena.
Every case must return `Failed`, leave the prior published pair byte-identical, remove temporaries, and report zero open
files. Add a short-write/sync/close/first-rename/second-rename matrix using the HAL storage stub.

Cancellation tests use callbacks that trip during source CRC, first/middle/final BMP row, PNG scanline, scaled-row flush,
JPEG MCU callback, and immediately before publication. Assert bounded callback progress, `Cancelled`, no handle leak, no
temporary pair, and preservation of old art. Activity tests hold a render operation, request navigation/sleep/USB transfer,
and prove `requestBackgroundCancellation()` runs before ActivityManager attempts `RenderLock`, then the locked
`prepareToSuspend()` returns false until file ownership drains and permits exactly one transition. A stale completion
generation must not update a new selection. Add the lock-race regression explicitly: capture one batch generation, block
render on `RenderLock`, issue cancellation, release the lock, and prove neither the current candidate nor any following
center/side/grid candidate opens a source. A later explicit relevant render may capture the new generation and try once.
Sleep-clock tests use the 2,500-ms policy with a fake wrap-safe clock, prove validation/CRC consumes the same budget as
decode/publication, and prove expiry of one variant prevents every later variant in that sleep-entry batch.

Finally run the native suite serially, the manga simulator smoke path with rapid Home/Grid input during generation, and
serial C3/S3 firmware builds. Hardware checks should use large valid JPEG/PNG first pages, cancel near the final rows, enter
sleep during generation, and immediately enter USB/file transfer afterward. Expected evidence is responsive input, retained
placeholder/old cover, no partial cache, no open-handle/storage error, one framebuffer, and stable free/largest internal
heap across repeated cancellation and re-entry.

## Rejected alternatives

A dedicated cover worker is not selected because Home and Grid already execute generation on the renderer-owned task; it
would consume another task stack and create cross-task state and SD coordination. Moving renderer calls to a cover worker is
invalid because renderer state remains owned by the render task. Polling input directly inside codec callbacks is also
rejected: it couples reusable converters to `MappedInputManager` and cannot cover lifecycle, sleep, or transport revocation.
