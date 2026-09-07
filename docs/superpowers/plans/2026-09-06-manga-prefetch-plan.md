# Task 8 — cancellable bounded manga prefetch

Read `2026-09-06-manga-prefetch-audit.md` for the verified source map and failure
cases. Binding: handoff prefetch requirements and manga-port-design. No commits,
pushes, SDK changes, second framebuffer or implicit PSRAM. Preserve all foreground
EPUB/dictionary behavior and existing format/version compatibility.

## Task 8a: renderer-free cancellable decoding

Add a cache-only entry point to the existing image decoder interface. It consumes
explicit exact output geometry, screen extents, path, policy and a function-pointer
cancellation callback/context. Share decoding internals with the foreground entry;
the cache-only branch must not access a renderer or framebuffer. Preserve gray
quantization and scale mapping so cache bytes match foreground generation.

Check cancellation before setup, at every JPEG MCU/PNG row, after decoding, during
streamed source/payload checksums, and before cache publication. Cancellation must
remain unconditional after two-thirds completion. Track early callback cancellation
explicitly because JPEGDEC can report decode success after a callback abort.

Cache-only success requires the cache stream to start, every write to succeed and
final sync/close to succeed. Cancellation/failed writes must close handles and
remove temporary output. Foreground cache failures must retain normal BW fallback.
Keep existing decoder call sites compatible. Add optional cancellation to manga
fingerprint/cache validation/publication and BMP row production using one small
callback contract. No per-pixel callback; use block/row/chunk boundaries.

Test cancellation before/early/middle/late, cache finalization failures, partial
cleanup and renderer-free byte parity with real decoders where available. Native
test dependencies must be local/offline. Do not label stub decoder tests as real
JPEG/PNG verification.

## Task 8b: one-slot worker and activity ownership

Add one fallibly created worker, one copied/reserved request and one small result;
no unbounded queue, shared PageView, renderer mutation or worker-side activity APIs.
Use explicit atomic release/acquire state transfer and generation invalidation.
Prepare candidate paths from the foreground-owned MangaBook only when idle; worker
consumes copied paths. Snapshot base/rotated viewports and orientation under
RenderLock; share the exact geometry/identity builder with foreground rendering.

Warm the useful first/next existing panel and next overview after idle dwell.
Support current and legacy paths, skip absent/already valid targets, and back off
after memory/resource failure. A failed task allocation disables speculation only.
Budget decoder + cache band + source rows + task stack + internal-RAM reserve;
inspect largest free block as well as total heap on C3. No hardcoded assumption
that a 60 KB free heap makes every codec safe.

Foreground intent cancels speculation before any image/page operations. While the
worker is draining, retain pending navigation/menu/render intent and keep polling
input; do not block the input loop on a full decode. Gate source/cache ownership
until all worker handles and decode allocations are released. `RenderLock::peek`
and per-call storage mutexes alone are not sufficient. Never hold a nonrecursive
storage lock around nested decoder HAL calls.

Add a default no-op activity readiness/quiescence hook to preserve pending Push,
Replace and Pop transitions until the current activity's background files are
closed. Cover global settings/frontlight/alerts/sleep, not merely reader menu
call sites. onExit defensively cancels/joins before saves or freeing ownership;
never forcibly delete a task holding files or free its context on timeout.

Tests: one-slot/stale-generation ownership, coalesced pending intent, repeated
navigation/rotation/manual refresh, child pushes, replace/pop/sleep and shutdown;
same-asset foreground/worker exclusion and cold/prefetched identity/pixel parity.
Native CTest remains sequential. Build simulator/C3/S3 serially through parent.

## Hardware

On X4, pause to warm a next panel/page then advance; rapidly reverse, rotate, open
lookup/settings and exit/sleep during large JPEG/PNG work. Check responsive input,
cache-hit/cancellation logs, identical framing/gray shades, no duplicate-reader
errors, and free/largest heap plus worker stack watermark across repeated cycles.
Repeat on S3 when hardware is available; no unmeasured latency or heap claim.
