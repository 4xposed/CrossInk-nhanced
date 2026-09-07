# Manga library integration — Task 6

Continue in place on `matcha_features`; no commits or pushes. Preserve dictionary work and SDK pin.

## Implementation

- Add lightweight MangaBook index/metadata/cover modes, skipping reader page buffers and optional TOC.
- Register opened manga in recents with bounded UTF-8 metadata, shared CRC cache identity and a versioned thumbnail template.
- Generate first-full-page covers through existing JPEG/PNG converters or bounded row-streaming BMP scaling; validate warm caches and retry transient failures.
- Wire Home, grid, Continue and sleep covers/progress/stats through shared helpers.
- Track reading time and qualifying physical-page turns through existing stats stores; panels are not separate pages.
- Capture manga identity before directory deletion, clean state only after content removal succeeds, and preserve durable progress/bookmarks/stats during cache clearing.
- Folder move/rename UI is not currently exposed; do not broaden network file contracts in this slice. Migration remains a documented acceptance gap for a future exposed folder operation.

## Ownership and verification

Parent: lightweight adapter, recents metadata/progress, CMake, integration verification and ledger.
Cover worker: new cover helper/tests and sleep cover integration.
Reader/library worker: reader stats and Home/grid covers (excluding grid deletion).
Actions worker: BookActions/cache cleanup, file browser and recent deletion handlers, progress cleanup APIs/tests.

Run native tests sequentially, focused cover/progress failure tests, default and Sticky firmware builds, button/touch manga smoke and existing EPUB/dictionary smoke on rebuilt simulators. Measure C3 OTA headroom (Task 5 had 24,272 bytes). Verify new hardware behavior separately from simulator evidence.

Hardware confirmed by user after Task 5 flash: manga opens, resumes the same panel, Back returns to full page, and wide-panel rotation works. Task 6 hardware checks: Home/Recent covers and progress, reopen from recents, reading time after dwelling on pages, cache clearing preserves resume, and folder deletion removes only the selected manga.
