# Task 7 — Manga image geometry and pixel cache

Continue on `matcha_features`, preserving dictionary work and SDK pin; no staging,
commits or pushes. Binding requirements: manga-port-design and manga-port-handoff.

## Design and ownership

| Work | Produces | Consumes / verification |
| --- | --- | --- |
| Pure geometry | Validated integer fit/centering and aspect rotation | Runtime oriented insets; exact rectangles tested with portrait/landscape/square/odd sizes and invalid values |
| Pixel-cache envelope | Versioned source/settings/geometry identity, complete payload validation, temp publication, bounded row reads | Existing decoder raw 2-bit output; same-size source replacement, truncated/corrupt/mismatched payload, write failures tested |
| Foreground integration | One decoder pass produces cache; cached BW/gray planes and restored BW baseline | Same geometry and pixels on cold/warm runs; mono BMP stays one BW refresh; failures retain a usable BW fallback |
| Review/build | Failure/lifetime review, native and simulator checks, C3/S3 size | No SDK/dictionary replacement, no second framebuffer |

Ruling: keep the existing decoder's private raw cache format unchanged for EPUB;
wrap manga output in an independently versioned validated publication contract.
This avoids invalidating dictionary/EPUB work and keeps firmware growth narrow.

Ruling: use HAL `FsFile` alias and explicit closes from AGENTS.md; the local HAL
skill's conflicting no-close/FsFile wording is superseded by the canonical guide.

Ruling: do not allocate a BW framebuffer backup for manga gray passes. Stream the
validated pixels again into the existing BW framebuffer and restore the display's
differential baseline after the gray update. A cache/read failure falls back to BW.

Ruling: Task 7 remains foreground-only. Task 8 adds cancellation, cache-only worker
APIs and same-source coordination; no worker may touch renderer/activity state.

## Verification

Write behavior tests before implementation. Rebuild native suite, run CTest -j1;
exercise pixel/geometry tests with bounds, corruption, identity changes and file
failures. Extend simulator BMP fixture to include grayscale and assert cold/warm
framebuffer hashes. JPEG/PNG simulator stubs cannot establish real decode parity.
Build default/C3 and Sticky/S3 serially (PlatformIO shared dependencies). C3 begins
with 11,072 bytes OTA headroom: measure and resolve fit without deleting dictionary
features or changing user fonts. Final hardware checks compare cold/warm images,
orientation, grayscale, ghosting, resume and repeated-turn free/largest heap.

## Hardware check after flashing this revision

On the X4, open the existing manga folder through Books. Compare a cold full page
and wide panel with the same images after moving away and returning: framing,
rotation direction and gray shades must agree. Toggle Rotate Panels, return to
overview with Back, use Refresh Screen, exit/reopen and sleep/wake. Menus and the
next page must not show the previous gray image as a stale differential baseline.
No book/state reset is required; the new `pixels_v1_*` files are disposable and
separate from progress/bookmarks. If testing recovery, truncate only a disposable
pixel file and confirm the source regenerates. Repeat on an S3 touch device and
record free/largest internal heap after repeated turns; physical ghosting and
JPEG/PNG quality cannot be established by the simulator BMP checks.
