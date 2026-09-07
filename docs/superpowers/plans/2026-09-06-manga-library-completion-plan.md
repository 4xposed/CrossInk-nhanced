# Remaining library acceptance work

Read `2026-09-06-manga-completion-gap-audit.md` for exact call sites. These are
requirements from the original handoff, not optional polish. Preserve existing
dictionary/font work, SDK pin, binary book format and all user data. No commits,
pushes, destructive cleanup or new unrelated folder-management UI.

## Task 10a: covers, cancellation and fallible PNG allocation

Use `2026-09-06-manga-cover-design.md` for the verified caller map and ownership
model. Reuse the render task and add a separate lock-free cancellation-request
hook before acquiring RenderLock; retain the existing locked suspension-readiness
contract. A sleep budget must include source validation/CRC and remain explicitly
cooperative, with fallback and actual elapsed-time diagnostics.
Use the design's 2,500 ms whole-sleep-attempt policy budget (half the configured
five-second watchdog window), explicitly unmeasured and not a hard deadline.
Capture one monotonic atomic cancellation generation per render batch; never
clear cancellation before each cover or restart later sizes/items after mismatch.

Propagate the existing lightweight cancellation contract through MangaCover source
CRC, BMP conversion rows, and JPEG/PNG ToBmp conversion callbacks. Callers must
actually revoke work on input/lifecycle changes; adding an unused callback alone
does not satisfy cancellation. Keep rendering on its owner task. Where synchronous
generation blocks main input, use a bounded owned generation job or resumable
workflow, preserving placeholder/previous valid art and avoiding retry storms.
Define sleep-generation cancellation/quiescence explicitly. Do not share a worker
with an active reader without clear lifecycle and source-file ownership.

Harden the reachable PNG one-bit converter allocations, including all internal
Atkinson error rows and scaling arrays. Use fallible ownership and cleanup; an
outer nothrow allocation does not fix throwing allocations inside a constructor.
Preserve image pixels and existing EPUB/sleep cover callers. BMP covers already
disable the shared decoder's error diffusion; do not change that path needlessly.
Test real JPEG/PNG covers, every allocation/finalization failure, cancellation
including final rows, valid temporary publication and repeated owner teardown.
The real-codec baseline also exposed rejection of valid contained output:
an 80x120 source requested at 200x390 produces 200x300, but current validation
requires the requested dimensions exactly. Preserve converter pixels and accept
the correct bounded contained geometry with strict BMP header/extent checks and
requested-size/source identity; test publication/reuse and malformed/oversized
dimension rejection. Preserve 50 pre-change real-codec vectors across actual UI sizes.

## Task 10b: reading time language attribution

Use `2026-09-06-manga-language-stats-design.md` for the concrete bounded format
and integration contract: fixed eight-bucket language summaries, streamed local
730-day appendix, and summary-only Nearby transfer. Remote daily-language history
is unavailable and must not be presented as aggregated. Preserve existing timers
and TXT behavior; this work does not add a replacement statistics subsystem.

Extend the existing stats system with bounded persisted language and day accounting
for exactly the same committed intervals as total reading time. Support mixed and
unknown languages, normalized tags, midnight spans and later metadata changes.
A current-language label alone cannot attribute historical durations. Do not import
Matcha's entire stats/UI or expand every stack-allocated GlobalReadingStats value
into a large history array. Use bounded/streamed storage and justify allocations.

Version changed binary layouts, retain legacy reads, preserve unknown historical
time, and keep clear/reset/cache preservation and synced aggregation consistent.
Expose useful language totals through existing stats UI. Ensure existing readers'
totals are unchanged; manga supplies meta.bin language. Test old/new round trips,
mixed/unknown language spans, day boundaries, saturation and reset/aggregation.

## Task 10c: transport folder moves and deletion

`2026-09-06-manga-transport-design.md` maps the existing stores and proposes
bounded staged migration with restart recovery. Inspect its concrete ownership
and publication requirements when preparing the implementation brief; existing
void/best-effort bookmark and recent-store helpers cannot acknowledge durable
migration success.

The on-device and HTTP rename/move UI rejects folders; do not add new controls.
WebDAV MOVE and USB serial RENAME do support directory moves, including ancestors
of indexed manga. Snapshot affected manga book paths with bounded storage before
moving. After successful movement, migrate durable progress/bookmarks/stats and
recent/resume references to the derived destination. Preserve state when movement
fails; detect collisions and never overwrite unrelated destination state. Ensure
partial metadata migration remains recoverable. Pixel/thumbnail caches are
disposable, but reading stats in those cache directories are not.

Task 9 adds use of existing `dictionary_history.txt` and per-book `dictionary.bin`
inside the manga cache directory. These are durable history/routing choices:
include them in move collision checks, staged migration, and cache-clear
preservation. Candidate scan files remain disposable. Current BookCacheUtils
preserves history but omits dictionary.bin; cover that distinction with tests.

Reuse bounded pre-delete bookkeeping for HTTP and USB recursive deletion, matching
the device browser's metadata cleanup. Do not broaden WebDAV empty-directory-only
deletion. Coordinate active reader/prefetch/dictionary lifetimes before file
mutation. Raw SD/USB mass-storage renames outside firmware remain documented
path-identity limitations; firmware cannot invent events it never observes.

Test direct and ancestor moves, failures/collisions/partial recovery, direct and
recursive deletes, progress/bookmark/stat preservation and recent/resume updates.
Use only disposable fixtures for transport tests; never the user's manga/books.

## Task 10e: complete pinned manga menu, status and lookup shortcuts

This task is required before the final gate. Use
`2026-09-06-manga-menu-completion-audit.md` as the behavior inventory; none of
its five missing commands, status affordances or shortcut routes is optional.
Task 9b must be present so QR and both shortcut forms can reuse the owned
current-view OCR source and unified lookup activity. Task 9c must be complete
before persistent scan reuse is enabled. Coordinate cache deletion with Task
10c: preserve `dictionary_history.txt` and per-book `dictionary.bin` along with
progress, bookmarks and statistics; candidate scans, pixel caches and covers
remain disposable.

The root handoff requires Confirm in panel mode to open current-panel lookup
directly. That input routing and its pending/coalesced-prefetch-drain tests belong
to Task 9b. Task 10e must preserve it: panel-menu testing enters through the
touch menu gesture or global reader-settings/menu escape path, or returns to the
overview before using button Confirm. Do not make panel Confirm open the menu.

Start with native menu/state tests. Replace positional manga menu integers with
a bounded enum/model and expose every pinned action on button and touch paths:
the current navigation, bookmark, Panels Only, panel rotation, orientation,
Home, lookup, stored translation and history actions, plus filtered reader
settings, Off/1/3/6/12-per-minute auto turn, screenshot, confirmed cache delete
and offline OCR QR. Existing global shortcuts and Home/browser cache actions do
not waive the reader-menu rows. Reuse the current popup unless its measured row
capacity requires the existing paged/tab menu widget; do not import unrelated
EPUB footnote, sync, clipping, pace or render-mode commands.

Build the settings child from shared `SettingInfo`/Settings widgets but include
only controls the manga path actually consumes. Initially this is reader touch
disable and applicable mapped-input controls; keep Panels Only, panel rotation
and orientation in their existing menu rows. Generic status bar, fonts, margins,
dark mode, EPUB images/styles, bionic/guide reading, dictionary and indexing must
stay hidden unless this same task wires and tests their manga behavior. Quiesce
prefetch before the child and restore the same page, panel, orientation and
per-book flags on return.

Implement auto turn as session state with wrap-safe `millis()` deadlines and
`preventAutoSleep()` while active. A directly testable automatic-forward helper
must call the equivalent of pinned `nextPage()` from overview, skipping entry
into that page's panels; from panel view it follows next-panel navigation and
crosses pages normally. Panels Only and pages without an overview may resolve to
the first real crop. Reuse page-load/progress/completion/stat bookkeeping.
Confirm, Back, touch-menu, lock, suspend, exit and child launch cancel; competing
manual turns are ignored. Busy render/decode/grayscale/prefetch work defers and
re-arms one deadline, with no catch-up burst. The menu shows Off or the active
rate; do not add a per-second e-ink countdown.

Add the pinned overlays inside the oriented safe viewport: overview shows
`page/total`; panel mode shows `panel/count  page/total` at bottom right and the
translated Panels hint at bottom left. Use one bounded status-layout/draw helper
from the BW, LSB, MSB and restored-BW call sites in
`MangaReaderActivity::displayImageGrayscaleLocked()` and from the monochrome
path. The encoding is not the same in every pass. `GfxRenderer::fillRectImpl`
defines the framebuffer as zero=black and one=white
(`lib/GfxRenderer/GfxRenderer.cpp:1532`), while image/font grayscale code uses
LSB/MSB buffers as gray-selection masks (`:55`, `:865`, `:2071`). Therefore:

- in the initial and restored BW frame, paint an opaque white rectangle and
  black 1-bit label glyphs;
- in each LSB/MSB mask, clear that rectangle to zero so neither its white
  background nor black glyphs are selected for a gray transition; do not copy
  the BW rectangle bits into the masks;
- after replay, retain the labeled restored BW frame when calling
  `cleanupGrayscaleWithFrameBuffer()` (`GfxRenderer.cpp:3124`) so the driver's
  next differential baseline matches the visible page.

The current sequence to extend is manga BW display, precondition, LSB replay and
`copyGrayscaleLsbBuffers()`, MSB replay and `copyGrayscaleMsbBuffers()`, gray
display, BW replay, then cleanup (`MangaReaderActivity.cpp:913`). Deferred panel
gray completion must use the same mask-clearing helper. Do not allocate or store
a second framebuffer.

Defer screenshots until the popup has closed and the labeled manga frame has
been restored, then reuse `ScreenshotUtil`; supply manga page/progress metadata
through `getScreenshotInfo()`. Cache deletion uses `ConfirmationActivity`, saves
pending durable state, cancels/drains workers and closes files before
`clearBookCachePreservingUserState(folder)`, reports failure, and returns Home
only after successful deletion. QR reuses `QrDisplayActivity` with selected-panel
OCR or all-page OCR in stored order, newline-separated and offline. Cap assembly
at the QR encoder's 2,953-byte UTF-8 boundary before growing the string, report
truncation, and make the existing QR work-buffer allocation fallible. Empty OCR
shows the manga no-OCR message; translation text is never substituted.

Override both reader shortcut forms used by ActivityManager so configured
short-power word lookup and Quick Actions/chord lookup enter the same Task 9b
current-view flow as the menu. Reject them while modal, locked, suspended or
draining foreground cancellation, and preserve the global screenshot-chord
release suppression.

Native tests must cover stable menu/action mapping, all auto rates and
cancellation/defer/wrap/end transitions, overview-page skipping versus panel
steps, safe-viewport status layout in all orientations, and exact patch bits in
BW, LSB, MSB and restored BW. A renderer fixture should seed nonzero gray masks,
apply the helper, and prove both mask rectangles become zero while BW background
and glyph pixels remain one/zero respectively; then exercise
`displayGrayBuffer()` and cleanup through the real call sequence. Also test QR
ordering/empty/malformed/multibyte-boundary/OOM, screenshot timing/metadata/write
failure, cache preservation/failure, filtered settings, and both shortcut enum
routes. Simulator tests open every row from overview with button Confirm and from
panel mode through the touch/global menu routes, verify panel Confirm enters
lookup, compare cold/warm and mono/gray status pixels, and verify cache regeneration.
Hardware acceptance on C3 and S3 covers all rates, rapid cancellation during
JPEG/PNG/BMP and prefetch, QR and screenshot SD failures, rotated/inset labels,
ghosting, free/largest heap and task watermark.

## Task 10d: final gate

Run the full handoff review after Tasks 10a-10c, Task 10e and OCR/prefetch. Record final builds,
native and simulator tests, actual image sizes and available hardware observations.
User data and dictionary backup remain separate from test fixtures throughout.
