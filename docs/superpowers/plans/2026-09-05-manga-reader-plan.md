# Manga reader integration — Task 5

Continue the approved manga implementation plan on `matcha_features`; no commits,
pushes or uploads. Preserve dictionary and SDK work.

- Add allocation-free, tested page/panel transitions, including panel-only books,
  missing crops, bounds and stale resume positions.
- Add a small versioned per-book progress/settings store outside disposable caches.
  Reuse the reader save debouncer and flush on exit.
- Add a foreground activity using the existing single framebuffer and image decoders;
  serialize MangaBook buffer mutations and drawing with RenderLock.
- Add bounded chapter/bookmark selection using existing FreeInkUI list components.
- Connect marker-folder selection in Books mode and ReaderActivity dispatch. Other
  library metadata, thumbnails and destructive actions remain Task 6.
- Build native tests and simulator/C3 firmware; record limitations and hardware checks.

Ruling: skip missing panel crops during navigation, retaining the pinned reader's
normal overview-back-to-previous-overview behavior. This prevents blank intermediate
steps while preserving available crop order.

Ruling: rotation and panels-only preferences are per-book as required by the handoff.
Durable progress is separate from disposable render caches so cache deletion cannot
silently erase a reading position.

Ruling: initial foreground rendering uses existing BW/dither decoders. Full grayscale,
shared cache geometry and cached/fresh parity remain explicitly tracked in Task 7.

## Review and verification in progress

- Navigation helper: 12 native tests, ASan/UBSan passed.
- Progress store: 7 tests including write/sync/close/replacement failure recovery.
- Independent reader review found failed-save retry churn and quadratic TOC seeks.
  Fixed with a 30-second retry delay (pending state retained) and a cached next
  TOC record offset. Two adapter regressions cover sequential/backward access and
  failure recovery. Scoped re-review found no remaining blocker.
- Full native suite after the cursor change: 559/559 passed sequentially.
- Simulator build and existing EPUB/dictionary smoke test passed.
- Initial full default/C3 build passed, OTA image 6,529,200 bytes with 24,400 free.
  Final source changes will be rebuilt before recording the final figure.
- No upload, SD-card write, commit or push has been performed.

## Completion — 2026-09-06

Task 5 is complete. Final C3 firmware: 6,529,328 bytes (24,272 free in OTA slot);
final Sticky/S3: 6,317,520 bytes (236,080 free). Both final simulator builds and
button/touch manga smoke pass, including real Books-folder selection, chapter/
bookmark/percent menus, manual refresh, bookmark reload and exact progress/settings
recovery. Existing EPUB/dictionary smoke passes; native suite 559/559.

Hardware image/ghosting/heap verification remains pending as described in the review.
Continue at Task 6; the complete Matcha port is not yet finished.
