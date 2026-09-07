# Manga port completion

The user requested continuous work until feature complete. Continue in place on
`matcha_features`; never commit/push/stage or delete the checkout. Preserve the
dictionary implementation, SDK gitlink, all existing fonts and book-format
compatibility. No cloud OCR, credentials, model downloads or live paid calls.
Use the handoff's full checklist, not merely the task summaries, for final review.

## Completion sequence

1. Create flash room by sharing identical immutable built-in font tables. Current
   linked C3 ELF contains about 135,533 duplicate constant bytes. Preserve every
   source font, bitmap, metric, interval, kerning value, font ID and choice; prove
   generated table equivalence and measure the linked image. No global unsafe
   constant-merging flags and no font removal. Original headers remain sources.
2. Implement Task 8 from the prefetch audit: renderer-free cache decoding,
   unconditional cooperative cancellation, one bounded worker slot, shared exact
   geometry, explicit foreground ownership and lifecycle quiescence including Push.
   Test cancellation/failure/ownership and preserve foreground decoder callers.
3. Implement Task 9 through existing unified dictionary interfaces, with owned OCR
   text/rectangle views, translations, word selection, scan identity/cache,
   saved/nested lookup/font restoration, and touch/button support. Resolve legacy
   crop geometry without altering existing Matcha book files.
4. Review the full handoff for gaps; build C3, Sticky, X4 Pro and simulator profiles,
   run native tests sequentially, converter offline tests and app-flow regressions.
   Complete available X4 hardware checks and safely restore the user's Japanese
   dictionary backup after inspecting both paths. Preserve the backup itself.
5. Record precisely which physical acceptance checks remain unavailable; never
   describe unavailable S3 hardware or image quality checks as passing.

## Review decisions

- Ruling: share byte-identical built-in tables at build time rather than removing
  fonts or changing SD font formats. This preserves content but adds a generation
  step; equivalence/reproducibility tests and firmware size checks must gate it.
- Existing work is deliberately uncommitted. Review packages must include working
  files and new untracked source, rather than relying on a commit-only diff.
- Ruling: legacy v2 crop margins cannot be recovered exactly. Panel lookup uses
  that panel's ordered OCR on the full-page background when available, otherwise
  a rendered text area. Highlight whole OCR blocks rather than invented glyph
  boxes. This preserves selection accuracy at the cost of leaving the crop zoom
  temporarily; the original panel view must be restored on exit.
- Ruling: language statistics use eight bounded totals and streamed local daily
  history. Nearby transfers compact totals within its existing packet limit;
  remote daily detail is unavailable. This keeps C3 memory bounded and avoids a
  new transfer protocol; older firmware cannot import the new summary version.

## Progress

- Task 7 was flashed to X4; user reported "all good" after the requested rotation/
  shades/reopen check. This is a user observation, not a measured heap or S3 matrix.
- Prefetch and OCR audits are underway; feature implementation follows reviewable
  briefs with ownership boundaries. Native baseline: 599 passing tests.
- Font pooling: complete after one integration fix/re-review. Six host tests pass,
  including compiled default/noemoji equivalence and real compiler include-order
  regression. C3 compiler dependencies name generated headers and the ELF contains
  pooled symbols. Font static payload fell from 2,280,555 to 2,154,062 bytes:
  **126,493 bytes saved without changing contents**. Intermediate firmware with
  initial decoder API work is 6,426,624 bytes, leaving 126,976 bytes free; this is
  not a final manga-completion firmware or a new hardware upload.
- Task 8a renderer-free cancellable decoding passed independent review and 39
  focused tests using real codecs, including pre-change foreground golden hashes.
  Full native suite passed 623/623; simulator and C3 builds passed. C3 image was
  6,426,848 bytes with 126,752 bytes free. These results precede Task 8b changes.
- Task 8b worker/lifecycle integration is underway. Its first simulator build found
  unsupported tick-conversion macros; the implementer is correcting those and
  removing worker setup string allocations so OOM disables speculation safely.
- OCR plan records the separate owned-source, shared-activity and verified scan-
  identity work. The remaining-library plan tracks cover cancellation/PNG OOM,
  language/day reading attribution and transport move/delete state consistency.
- X4 reconnected on /dev/cu.usbmodem1101 and reports Task 7 firmware. The original
  Japanese dictionary backup is intact (272,117,145 bytes across nine files).
  Automatic approval review rejected exporting it to local temporary storage;
  explicit user approval was requested. No backup transfer or restoration occurred.
- Task 8b review: changes required. Fix round 1/5 underway for completion-boundary
  pending-input ordering (opposite directions and duplicate menu intent). Native
  634/634 and normal mono/gray/EPUB simulator flows pass. Deterministic held-file
  stress passes Push/Pop/Replace/manual-refresh/main sleep, then fails at a later
  chapter-menu transition. Full stress and scoped re-review remain required.
- Task 8b: fix round 1/5 addressed the pending-input finding; scoped spec and
  quality review pass. Final deterministic grayscale stress passes at 32.096 s,
  including real-reader first/last-boundary and menu-drain checks. Full native
  suite passes 637/637. Software integration complete; final C3 build is running,
  and S3 builds/physical acceptance remain in the final gate. Task 9a started.
- Task 9a: complete, independent spec/quality review pass. Sixteen new OCR tests
  and all 285 dictionary regression tests pass. Sticky build passes with prefetch
  and adapter: 6,215,936 bytes, 337,664 free. X4 Pro build is running. Task 9b
  shares the existing lookup activity; external persistent scans stay disabled
  until Task 9c verifies dictionary identity.
- X4 Pro also passes: 6,312,624 bytes, 240,976 free. Task 9b implementation is
  underway. Task 8 checkpoint remains unflashed because the USB device vanished
  before its pre-flash status check; no dictionary or font data was changed.
- Task 9b intermediate integration: simulator build, expanded OCR flow (25.663 s),
  EPUB flow (3.314 s), and all 659 native tests pass. The original OCR smoke failure
  was a case-sensitive fixture index ordering mismatch with StarDict's comparison;
  a regression reproduces the exact second token and corrected ordering. No
  dictionary engine fix was needed. Empty-OCR/translation-only, touch and forced
  teardown gates remain in progress before independent review.
- The full reader contract also requires direct panel-Confirm lookup and the
  remaining menu/status/shortcut features. Panel Confirm is in Task 9b; Task 10e
  is now explicitly required before the final gate. Grayscale status patches use
  BW labels and cleared gray-selection masks, not identical bits in every plane.
- Latest USB enumeration, including outside the sandbox after the user's
  "connected" message, still has no reader endpoint. No firmware was flashed.
- Task 9b: complete with independent spec/quality approval. Final button smoke
  passes28.742s, touch28.997s, EPUB3.360s. Missing OCR/dictionaries, translation-only
  data, crop-only fallback, repeated Confirm during prefetch drain, forced exit,
  exact clipping, history and restored framebuffer/orientation are covered.
  Two final fixture errors (auxiliary book placement and Back-zone swipe origin)
  were corrected without changing production dictionary/input algorithms.
- Task 9c is now implementing canonical full-index scan identity. It preserves
  the old inexpensive signature API and hashes outside RenderLock without
  competing with the dictionary worker. Strict verification may make persistent
  caches unhelpful for a large dictionary when fresh scanning finishes first;
  first-definition responsiveness and correctness take priority.
- Task 9c: complete with independent spec/quality approval, 675 native tests,
  actual cold/warm activity cache checks on button and touch simulators, and EPUB
  regression. All hardware builds pass: C3 6,448,928 bytes (104,672 free), Sticky
  6,232,096 (321,504 free), X4 Pro 6,328,832 (224,768 free). Reviewed C3 checkpoint
  is saved at `/private/tmp/crossink-task9c-firmware.{bin,elf}` and remains unflashed.
- Task 10a is implementing cover cancellation, fallible decoder/dither allocation,
  checked publication and valid contained-size cover acceptance. Fifty pre-change
  real-codec pixel vectors were captured before source edits. Language/day stats,
  transport state migration, and remaining menu/status/shortcut work follow.
- Task 10a complete after independent review and one fix round: MCG3 exact
  dimensions/checksum, crossed-pair recovery, cooperative cancellation, fallible
  dithering and sleep diagnostics. Final 702 native tests, simulator builds,
  grayscale/prefetch stress, touch OCR and EPUB checks pass. All firmware targets
  build: C3 6,454,752/free98,848; Sticky 6,238,912/free314,688; X4 Pro
  6,335,760/free217,840. Final C3 artifact saved at
  `/private/tmp/crossink-task10a-firmware.{bin,elf}`, still unflashed. Physical
  cover timing remains unmeasured. Task 10b language statistics is implementing;
  Tasks 10c, 10e and final 10d remain required.
