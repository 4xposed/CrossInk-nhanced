# Task 9a: owned manga OCR source — 2026-09-06

Implemented in place on `matcha_features`. No commits, staging, pushes, SDK edits, font changes, PIO commands, serial access or reader/menu wiring. Baseline snapshot supplied by the parent: `/private/tmp/crossink-manga-ocr9a-before`. Existing unrelated workspace changes preserved.

## Files and APIs

- `src/activities/reader/PageTextSource.h:37`: movable `OwnedLookupTextSource` owns glyph metadata only. `view()` borrows the owner's immutable array; `clear()` releases it; moving empties the old view. Existing `HorizontalPageTextSource` remains unchanged.
- `src/activities/reader/MangaPageTextSource.h`: `MangaLookupGeometry`, `MangaLookupClippingRange` and public adapter contracts.
- `src/activities/reader/MangaPageTextSource.cpp:151`: `buildMangaLookupTextSource(page, panel, geometry, out)`. `panel == -1` visits all panels in saved order; nonnegative values select exactly one panel. Two passes over length-aware borrowed format text; one owned allocation, optional smaller retry. The parent must keep its page backing buffer immutable until the child exits and any returned clipping is reconstructed.
- `MangaPageTextSource.cpp:112`: `mapMangaLookupBlock` maps whole OCR rectangles through metadata-to-image-fit scaling, all image/base orientation combinations and asymmetric base insets. Floor left/top and ceil right/bottom preserve enclosing pixel bounds. Wide intermediate arithmetic clips source and screen edges; empty/invalid bounds fail. Every glyph from a block uses that whole rectangle. Offscreen blocks retain text identity and have empty geometry; no fabricated character rectangles.
- `MangaPageTextSource.cpp:263`: `copyMangaLookupClipping` reconstructs exact original bytes into caller storage after source teardown, using the same lexical walker as construction. Pass the existing `DictionaryClippingRequest`'s four ordinal/byte fields directly into `MangaLookupClippingRange`. It validates UTF-8 boundary offsets, capacity and source block/paragraph identity. Cross-control, malformed-sequence or cross-block ranges fail closed; normal whitespace bytes between runs are preserved exactly. No allocation.
- `MangaPageTextSource.cpp:296`: `mangaLookupCacheFileName` returns a bounded leaf name such as `manga-ocr-0-overview.bin` or `manga-ocr-0-panel-0.bin`. Store inside the book's existing cache directory. Distinct decimal fields avoid integer packing collisions across pages 0..9999 and panels 0..254; invalid values fail.
- `test/japanese_dictionary/CMakeLists.txt`: links the adapter and existing MangaFormat/ImageGeometry into the existing real dictionary fixture executable.
- `test/japanese_dictionary/JapaneseDictionaryTest.cpp:6194`: 16 new tests (14 adapter tests and 2 real backend scanner fixture tests).
- `CHANGELOG.md`: one Added entry explicitly describing adapter support as ready for reader integration.

## Text, memory and fallback policy

Glyph metadata remains 16 bytes; the existing scanner's candidates remain 8 bytes. At most 1024 glyphs means 16 KiB for owned metadata plus at most 8 KiB of scanner candidates (24 KiB combined, excluding existing dictionaries, page bytes and other activity state). An allocation failure logs and retries 256 glyphs (4 KiB source plus at most 2 KiB candidates). No per-glyph/per-frame heap allocation, second framebuffer or retained text copy. Exact clipping uses caller-supplied output storage.

Ordinals identify lexical runs, never individual Latin characters. Unicode spaces and en/em dashes terminate runs. Controls, NUL, newlines, malformed UTF-8 and OCR block boundaries also terminate paragraphs. Strict UTF-8 decoding accepts valid 1–4-byte codepoints, rejects overlong/surrogate/out-of-range/truncated sequences, and treats each invalid byte as a hard synthetic separator. This prevents malformed replacement lengths from shifting clipping offsets. Content hashes include the scope, codepoints, run ordinals and paragraph boundaries with a domain/version prefix; geometry is deliberately excluded because scan candidates refer to source glyph indices.

When a limit is reached, only a useful prefix is published and `truncated` is true. An incomplete trailing Latin token is withheld; contiguous Japanese text retains complete codepoints so long Japanese blocks remain useful. A first Latin token larger than the entire available budget produces no usable source and returns OutOfMemory with `truncated == true`. Complete allocation failure also returns OutOfMemory with empty output. Invalid views, scope or geometry return ReadError. Empty valid sources return NotFound. Successful nonempty output returns Found.

**The activity integration must visibly mark truncated sources and bypass both loading and saving scan caches for them.** This task supplies the source flag/contract; it does not change the shared activity/cache implementation.

Legacy crop origin is never inferred from `panel.box`. Build with the full-page background's `ImageLayout`, retaining the selected panel text scope. If no full overview exists, set `textOnly`: `views.base` is the selectable text area and `cellWidth`/`lineHeight` define a fixed-cell layout. Render each nonsynthetic glyph in its emitted cell; spaces consume a cell, controls/blocks move to a new row, cells wrap at the right edge. Screen exhaustion follows the same visible truncation policy. The UI must choose cells large enough for its selected font or clip glyph drawing to the cell. This is a testable layout contract, not a second lookup UI.

## Verification

Local/offline configuration and native builds only:

```sh
cmake -S test -B /private/tmp/crossink-manga-ocr9a-test \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/private/tmp/crossink-manga-pixel-build/_deps/googletest-src \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build /private/tmp/crossink-manga-ocr9a-test --target JapaneseDictionaryTest -j 2
/private/tmp/crossink-manga-ocr9a-test/japanese_dictionary/JapaneseDictionaryTest
```

Result: **285 tests from 13 suites passed**, including all 16 new OCR tests and existing EPUB/Japanese/StarDict fixtures. Final output: `/private/tmp/crossink-manga-ocr9a-native.log`. `git diff --check` passed. Build warnings were existing GoogleTest char8 conversion and Dictionary.cpp unused constants; the adapter emitted no warnings.

Initial configuration demonstrated the missing adapter source. Later tests explicitly failed for the missing long-Japanese-prefix and allocation-retry behavior, then passed after implementation. One new fixture initially lacked the partial-name dictionary record required by existing scanner policy; the fixture was corrected without changing dictionary behavior.

Tests include a physical-coordinate oracle for all 16 image/base orientation combinations; fractional scaling, asymmetric insets and wide clipping; ownership after page bytes are discarded; exact UTF-8 byte reconstruction after owner teardown; malformed encodings/NUL/control/Unicode-space/block boundaries; four-byte codepoints; scope and hash changes; representative physical-page boundaries with every panel cache name; empty/invalid input; allocation failure and fallback; text-only wrapping and screen truncation; long Japanese prefixes and incomplete Latin suffixes. Real Japanese scanners cover longest matching, deinflection, grammar, names/honorifics, counters and partial katakana names, comparing candidate segmentation against the existing plain-glyph fixture. Real StarDict scanning preserves whole known and unknown words.

## Integration and hardware limits

Task 9b must move the owner into the existing shared lookup activity, choose full-page versus text fallback background, retain the immutable parent page and scope, copy the four clipping fields, provide caller-owned clipping storage, honor truncation before any cache load/save, and restore prior page/panel/base orientation on exit. Those lifecycle/UI requirements cannot be verified by this pure adapter task. Dictionary identity hardening remains Task 9c.

No device performance or memory improvement is claimed. Root owns firmware builds and serial testing. After Task 9b wiring, verify X3/X4 and S3 devices with overview and legacy-crop books: select OCR text under all orientations/insets, compare whole-block highlights against the displayed overview, verify text-only fallback without an overview, clip Japanese substrings and Latin words, press Back, and confirm prior panel/orientation restoration. Exercise >1024-glyph pages and low-memory entry: truncation must be visible and no partial scan cache reused. No EPUB/image cache reset is needed for this adapter-only change; remove the book's manga OCR scan files when testing later scan identity changes.
