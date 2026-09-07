# Task 10a independent completion review

Date: 2026-09-07

Scope: the Task 10a brief, binding cover design, completion report, accepted
progress rulings, and `/private/tmp/crossink-manga-cover10a-review.patch`. This was
a read-only source review. Reported tests were not rerun and firmware or hardware
behavior was not exercised.

## Verdicts

**Spec verdict: changes required.** The task implements the selected render-owner
model, pre-lock lifecycle cancellation, sticky batch revocation, whole-attempt
sleep budget, fallible codec allocations, and real-codec golden coverage. Two
cache/publication cases do not yet meet the binding requirements that contained
geometry be correct and previous valid art remain recoverable after every failed
publication.

**Quality verdict: changes required.** Resource ownership is generally clear and
bounded, and failure paths use automatic cleanup. The two high-severity findings
below are durable-state correctness gaps. The low-severity sleep diagnostic gap
does not block functionality but contradicts the promised evidence.

## Findings

### High — contained cache validation accepts arbitrary wrong aspect ratios

`validBmp()` accepts any positive dimensions inside the requested box if either
axis reaches the requested edge within one pixel
(`lib/MangaPanel/MangaCover.cpp:54-67`). For a 123x180 request, a structurally
valid 123x1 BMP passes. The identity check records source CRC/size and the
requested width/height, but no decoded source dimensions or expected contained
output dimensions (`MangaCover.cpp:85-99,310-325`). Consequently the same invalid
geometry is accepted on the cached path at `:333-338`.

This is weaker than the brief's “correct bounded contained geometry” and its
malformed-dimension rejection requirement. The added test changes width only to
0, 124, and a very large value (`test/manga_cover/RealCoverTest.cpp:407-417`); it
does not test an in-bounds but aspect-inconsistent width/height pair.

Validate the exact expected contained dimensions, including the documented
one-pixel converter rounding where applicable. The durable identity or a bounded
source-header probe must provide enough source geometry to perform that check on
a warm cache hit. Add cases such as 123x1 and 1x180 with otherwise valid headers,
extent, source identity, and file size, alongside the valid 200x300 result for an
80x120 source requested at 200x390.

### High — one failed backup rename can leave the old valid pair split and unrecoverable

During `promote()`, the old image is renamed to its backup before the old identity
is renamed (`lib/MangaPanel/MangaCover.cpp:235-243`). If the identity-to-backup
rename fails, rollback attempts to rename the image backup to the primary name,
but its result is ignored (`:240-242`). A persistent storage/rename failure leaves
the old image at `.bak` and its matching identity at the primary `.src` name.
`recover()` recognizes only a complete primary pair or a complete backup pair
(`:220-233`), so this crossed pair is not restored on the next attempt.

The fault matrix injects one failing rename call and therefore lets the immediate
rollback succeed (`test/manga_cover/RealCoverTest.cpp:243-280`). It does not cover
a failed operation followed by a failed rollback or reboot from the crossed
state. The binding design requires both backups to be restored if either
promotion step fails and previous valid art to survive every failed publication.

Handle and test crossed recovery states explicitly, or structure backup creation
so a failure cannot make the only valid pair unrecognizable. At minimum, simulate
failure of the second backup rename plus rollback failure, recreate the owner as a
reboot would, and prove the old image/identity bytes become a consumable primary or
complete backup pair without publishing the new temporary pair.

### Low — sleep log does not report the generation result or cache-hit state

The design requires policy/elapsed/poll-gap/stage/source dimensions, cache
hit/miss, result, and fallback. `logMangaCoverAttempt()` logs most of these but
receives only `cachedFallback` and emits `cancelled` plus `fallback`
(`src/activities/boot_sleep/SleepActivity.cpp:1155-1163`). Its callers discard the
controlled preparation result and infer the boolean from whether a cached path
exists (`:731-742,794-803,817-825,843-855`). Existing art after a failed or
cancelled regeneration is therefore logged the same way as a cache hit, and
`Failed`, `Cancelled`, `Cached`, and `Published` cannot be distinguished.

Retain the `ThumbnailResult` through `SleepCoverAssets` and include it in this
diagnostic. Keep the current fallback path field as a separate fact.

## Conforming areas

- `Activity::requestBackgroundCancellation()` is explicitly pre-lock and
  atomic-only (`src/activities/Activity.h:40-44`). Push, replace, and pop revoke
  immediately (`ActivityManager.cpp:400-408,612-632`), while the common suspension
  gate requests cancellation before constructing `RenderLock` and retains the
  locked `prepareToSuspend()` call (`BackgroundSuspension.h:3-10`).
- Home and Grid remain on the existing render task. `MangaCoverInput` revokes,
  drains the render lock, runs the main-side intent while cancellation remains
  sticky, and authorizes that exact generation afterward
  (`MangaCoverInput.h:6-25`). Home and Grid capture one batch before their loops
  and stop before following sizes/items on mismatch
  (`HomeActivity.cpp:670-675,705-743`; `RecentBooksGridActivity.cpp:257-262,264-314`).
- `MangaCoverWork` uses the accepted single-main-writer relaxed-load/release-store
  increment and acquire observation, without the rejected type-wide lock-free
  assertion (`MangaCoverWork.h:8-37`). Authorization cannot revive an intervening
  later lifecycle cancellation (`:22-25`).
- Home/Grid readiness reads render-owned `active` only under `RenderLock`; scoped
  cleanup clears it after generation. Their `onExit()` checks expose an ownership
  violation (`HomeActivity.h:134-136`, `RecentBooksGridActivity.h:21-23`,
  `RecentBooksGridActivity.cpp:366-370`). No new task or framebuffer is introduced.
- CRC polls every 256 bytes and BMP conversion polls rows
  (`MangaCover.cpp:72-83,111-154`). JPEG and PNG receive the borrowed cancellation,
  reject checked-output short writes, and test final boundaries. Publication
  begins only after source/output close, output sync and temporary validation
  (`MangaCover.cpp:351-382`).
- The sleep budget is created once on entry and starts lazily on the first manga
  attempt (`SleepActivity.cpp:494-496,731-735`). It is sticky across later calls,
  uses wrap-safe subtraction, and labels 2500 ms as an unmeasured cooperative
  policy bounded against half the configured watchdog interval
  (`SleepCoverBudget.h:9-17,20-52`). Synchronous `onEnter()` owns the attempt, so no
  cover worker or SD handle can outlive it.
- Dither objects and every inner row allocate through fallible ownership; PNG
  context, source rows, output row, gray row and scaling arrays are checked and
  automatically released. The task patch retains one renderer framebuffer and
  reported goldens cover 50 real JPEG/PNG conversions. No pixel-change issue is
  visible in static review.

## Cannot verify from this review

- The reported 19/19 real-cover tests, 9/9 cover tests, 7/7 bitmap tests, 693-test
  native suite, simulator builds, and pixel hashes were not rerun by instruction.
- The final C3 build after the atomic refinement, actual RV32 generated assembly,
  physical cancellation responsiveness, SD rename behavior, watchdog margin,
  heap stability, and task stack watermarks require the root build/hardware gate.
- Host owner tests exercise the production owner and gate with a host mutex, but
  do not instantiate the full Home/Grid UI with the FreeRTOS scheduler. Static
  inspection confirms the production call sites; timing remains a simulator or
  hardware verification item.

## Fix round 1 scoped re-review — 2026-09-07

Reviewed `/private/tmp/crossink-manga-cover10a-fix1-review.patch` and the appended
completion report against the three findings above and the accepted `thumb_v3` /
40-byte `MCG3` ruling. Reported 27/27 RealCoverTest and 9/9 MangaCoverTest results
were not rerun.

**Spec status: pass for this fix round.** Both High findings and the Low finding
are addressed. The fix implements the accepted cache-version ruling, documents
the byte layout, and adds the required publication/warm-vector coverage. No new
spec breakage was found in the scoped diff.

**Quality status: pass for this fix round.** Recovery candidates are digest-bound
before destructive changes, emitted geometry comes from the real codecs, and the
new checksum work reuses bounded storage. Remaining uncertainty is confined to
the root build and hardware gates listed below.

### Prior High: contained geometry — ADDRESSED

The cache is versioned to `thumb_v3`, so old permissive v2 artifacts are ignored
(`lib/MangaPanel/MangaCover.cpp:216-228`; `MangaCover.cpp:191-196` for public path
helpers). `MCG3` stores requested dimensions, exact emitted dimensions, full BMP
CRC, and source identity in a checked 40-byte layout
(`MangaCover.cpp:23-32`; `docs/file-formats.md:15-36`).

For new output, `emittedDimensions()` reproduces adaptive-contain policy from the
codec-reported physical source dimensions, including progressive JPEG eighth-scale
geometry (`MangaCover.cpp:112-130`; `JpegToBmpConverter.cpp:641-655`). Generation
then requires the temporary BMP to have those exact dimensions before recording
its complete-file CRC and publishing (`MangaCover.cpp:408-431`). Warm reuse checks
the sidecar's bounded output dimensions, exact BMP header/extent, BMP CRC, and
current source path/content/size identity (`:230-243,359-387`). This matches the
accepted sidecar design without a second parser or journal.

The regression now exercises structurally valid 123x1 and 1x180 replacements
(`test/manga_cover/RealCoverTest.cpp:496-509`). The 50-vector publication test
checks sidecar size/magic/requested/emitted fields, golden pixels, and subsequent
warm reuse for every fixture/size (`:625-661`).

### Prior High: crossed publication recovery — ADDRESSED

Recovery validates the primary pair, complete backup pair, and both crossed
image/identity arrangements. Each candidate must pass exact dimensions and the
stored complete BMP digest before any primary name is removed or renamed
(`lib/MangaPanel/MangaCover.cpp:230-277`). If either recovery rename fails, the
remaining complete or crossed arrangement is still one of those recognized on
the next attempt. Promotion continues to retain complete backups or a recognized
crossed state when immediate rollback cannot finish (`:279-306`).

Tests now cover persistent failure beginning at the second backup move followed
by owner reopen and recovery (`test/manga_cover/RealCoverTest.cpp:511-536`), reject
crossed candidates whose identity belongs to different pixels in both directions
(`:538-574`), sweep cancellation through recovery digest polling (`:575-605`),
and retry failures of either recovery rename (`:677-700`). These cases close the
double-failure/reboot gap from the first review.

### Prior Low: sleep result diagnostics — ADDRESSED

`ThumbnailDiagnostics` now defaults and resets `result`, and the controlled API
sets it on every cached, published, cancelled, or failed return
(`lib/MangaPanel/MangaCover.h:11-20`; `MangaCover.cpp:319-328,387,437-439`). Early
SleepCoverAssets cancellation explicitly records Cancelled while other preflight
failures remain Failed, and controlled results propagate through its bool-compatible
callers (`src/activities/boot_sleep/SleepCoverAssets.cpp:50-64,94-105`).

The sleep log emits the named result and a separate true cache-hit field in
addition to cancellation and fallback selection
(`src/activities/boot_sleep/SleepActivity.cpp:1155-1178`). The result regression
covers all four enum values (`test/manga_cover/RealCoverTest.cpp:606-623`).

### New-breakage review

No new blocking or non-blocking defect was found in this fix diff. The new sidecar
uses compile-time size/offset and target-endian checks (`MangaCover.cpp:23-32`),
and its documented offsets match the implementation (`docs/file-formats.md:15-36`).
The additional source/BMP CRC passes reuse the existing 256-byte stack buffer and
remain cooperatively cancellable (`MangaCover.cpp:74-110,230-243`); they add SD
work but no framebuffer, worker task, journal, or unbounded allocation.

Cannot verify here: the reported tests and 50 pixel vectors, root's broad native
suite and simulator/firmware builds, physical SD recovery behavior, C3/S3 heap and
stack stability, cancellation latency, or the unmeasured 2500 ms sleep policy on
hardware.

### Simulator runner addendum

**Accepted; fix-round spec and quality status remain pass.** The refreshed patch's
only additional change updates `scripts/run_manga_simulator_smoke_test.py:207-219`
from `thumb_v2` to the production `thumb_v3` names. It reads the adjacent sidecar,
requires exactly 40 bytes and `MCG3`, decodes the four little-endian requested and
emitted dimensions at offsets 12-27, compares them with the request and BMP header,
and compares the complete BMP `zlib.crc32` with the stored checksum at offset 28.
Those offsets and checksum coverage match `docs/file-formats.md:15-36` and
`lib/MangaPanel/MangaCover.cpp:23-32,230-243`.

The theme-specific Home assertion also checks `thumb_v3_151x226.bmp`, removing the
remaining stale v2 expectation (`run_manga_simulator_smoke_test.py:219`). No new
finding was introduced by this runner-only delta. Root's reported reruns were not
independently repeated here.
