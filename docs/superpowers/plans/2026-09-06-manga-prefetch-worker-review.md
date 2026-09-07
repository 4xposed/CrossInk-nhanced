# Task 8b independent spec and quality review

Reviewed 2026-09-06 against `.superpowers/sdd/2026-09-06-manga-prefetch-plan/task-8b-brief.md`, the worker report, `/private/tmp/crossink-prefetch-worker-review.patch`, and current supporting source. No implementation edits, builds, tests, serial commands, commits, or SDK/font/dictionary changes were made by this reviewer.

## Findings

### P2 — Merge new navigation with retained intent before executing either

`src/activities/reader/MangaReaderActivity.cpp:425` dispatches new directional edges through `move()` before draining `pendingMoves` at line 458. `move()` only adds to the pending counter when the worker is still busy (line 177). The loop can consume Finished at line 382, so a new edge in the completion iteration bypasses older retained intent.

Concrete reproduction: start at page zero's overview with an available first panel, hold a worker request, press Forward (pendingMoves becomes +1), then deliver Back on the iteration that consumes completion. Back executes immediately at the first overview and does nothing; the old +1 then advances into the first panel. Coalescing the two directions should leave the reader at the overview. At other boundaries this reordering can also trigger completion/progress side effects before an older move executes.

Related menu case: a retained `pendingMenu` followed by a fresh Confirm after completion enters `openReaderSettingsMenu()` directly at line 416. The successful open at line 234 does not clear the old flag, so after that menu closes or its child returns, line 439 can reopen the menu without another user request. Treat these as one pending-intent ownership issue: ingest the current input into retained state first, then drain from one place, and consume the menu flag whenever the menu actually opens. Add deterministic completion-boundary tests for opposing directions at the first/last boundary and duplicate menu-open intent.

## Spec verdict: changes required

The bounded renderer-free worker, generation invalidation, copied source paths, foreground/worker geometry sharing, and transition quiescence are implemented. The pending-intent requirement is not fully met because of the finding above. The requested complete stress acceptance is also outstanding: the latest supplied run fails at `MangaReaderSelection` after its forced lifecycle prefix passes.

## Quality verdict: changes required

No additional blocking ownership or memory issue was found in the reviewed worker and decoder adapters. The pending-input defect needs correction and regression coverage before acceptance. The stress run must finish successfully or its remaining failure must be explained and repaired; a passing prefix does not establish full integration success.

## Reviewed safety properties

- `lib/MangaPanel/MangaPrefetchState.h:13` publishes a copied request with release/acquire transfer; cancel only invalidates generation and does not make the slot reusable. Finished is consumed before foreground I/O can resume.
- `src/activities/reader/MangaPrefetch.cpp:144` returns from production, closes the cache and discards temporary files before publishing Finished. Decoder and BMP scratch owners have left scope. The worker does not access MangaBook, PageView, renderer, or activity APIs. Its final context access is the stopped release store at line 161; terminal joining does not forcibly delete a task with live files.
- `src/activities/reader/MangaReaderActivity.cpp:758` gates rendering before image operations, and `ActivityManager.cpp:225` preserves pending Push/Pop/Replace while quiescence is false. Suspension prevents reposting; child return resumes the reader. `src/main.cpp:911` defers pre-sleep persistence as well as the activity transition. Repeated readiness calls still cancel unconditionally.
- Bounded fallible path buffers remain alive throughout synchronous decoder/cache calls. The const-char adapters preserve existing string entry points, and the worker's borrowed output path is owned by its persistent cache object. No second framebuffer or implicit PSRAM dependency was introduced.
- Admission includes actual codec object sizes, cache band/source-row scratch, overhead and a 32-KiB internal reserve; largest-block checks supplement total free heap. The resident 8-KiB stack remains a real foreground cost, as documented. This review does not establish practical C3 admission rates or stack margin on hardware.
- Foreground and worker use `buildImageLayout()` plus `applyImageLayout()` with copied base/rotated viewport geometry. BMP versus JPEG/PNG scale/dither policy and cache identity are shared.

## Evidence and limits

Parent reports sequential native **634/634 passing**, simulator and C3 builds passing, and normal mono/gray manga plus EPUB flows passing. I inspected the latest `/private/tmp/crossink-manga-prefetch-worker-stress.log`: held-source Push/Pop/Replace/manual-refresh/sleep steps complete, main sleep preparation is reached after cancellation, but the run ends at 8300 ms with `Expected current activity: MangaReaderSelection`.

The held-source barrier is materially stronger than a fixed delay: its second fingerprint cancellation callback runs with the checksum source file open. Stress-only interception occurs after normal sleep preparation, so it avoids the simulator's wait-for-wake loop without bypassing the production quiescence gate. The later two Confirm taps (`SimulatorSmokeTest.cpp:666`) do not explicitly wait for the retained menu to become visible, so their failure is not independently sufficient to identify a second production defect. The script needs deterministic readiness around that transition while preserving cancellation coverage. The native worker tests establish cleanup at acknowledgment and real-codec production; they do not exercise the actual reader's completion-iteration input ordering.

Hardware verification after the fix: on X4, clear the relevant disposable manga pixel cache, warm a large JPEG/PNG, then reverse direction at the first/last boundary as cancellation finishes; confirm the retained net navigation and absence of unexpected menu reopening. Also repeat rotation, refresh, global settings/frontlight, exit and sleep while warming; check identical framing/gray shades, no duplicate-reader errors, and logged internal free/largest heap plus task stack watermark. Repeat on S3. No hardware result or S3 build result is claimed here.

## Scoped re-review — fix round 1

Reviewed `/private/tmp/crossink-prefetch-worker-fix1-review.patch` and the worker report's “Review fix round 1” section. Scope is the original P2 and new blocking regressions in that fix; no builds/tests were rerun.

**Prior P2: ADDRESSED.** Every directional edge now enters `PendingInput` before the single `moveLocked()` drain applies navigation, so a fresh opposite direction cancels retained movement before first/last boundary behavior runs. Menu requests coalesce and `takeMenu()` clears ownership before showing the menu. The reader polls completion and opens a retained menu before `menu.handleInput()`, addressing the additional confirmed failure where a Confirm press on that frame was previously lost. Already-open menus absorb duplicate open requests. Rotations similarly accumulate before application.

The tests now include actual-reader simulator assertions, not just the helper: acknowledged completion is held until injection of the opposing fresh edge, then actual page/panel state is checked at both bounds. Duplicate menu requests followed by a completion-frame Confirm must open chapter selection and return with the menu closed. The gates and reader inspection hooks are simulator-only; the worker still closes resources before publishing completion. No new blocking issue was found in this fix diff.

**Scoped spec verdict: pass for the reviewed pending-intent correction. Scoped quality verdict: pass, with final integration execution pending.** Parent reports the relevant native tests **14/14 passing** and simulator build passing. At review time the final `/private/tmp/crossink-manga-prefetch-fix1-stress.log` contains only its startup line; this verdict does not claim that run passed. Overall integration acceptance still requires the parent to record its final stress outcome and final firmware validation. Hardware acceptance remains as described above.
