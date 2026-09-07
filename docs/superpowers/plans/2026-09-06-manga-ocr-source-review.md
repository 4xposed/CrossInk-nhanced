# Task 9a independent review — 2026-09-06

Spec verdict: **PASS for the clarified Task 9a adapter scope.**

Quality verdict: **PASS. No blocking findings identified.**

Severity: none. This review does not approve Task 9b activity integration or hardware behavior.

Reviewed the task brief, implementation report, supplied Task 9a patch, and current source/tests. Read repository instructions and durable context; used CodeGraph before investigating the existing scanner, clipping request, and image geometry collaborators. Preserved the unrelated working changes. No implementation edits, builds, test runs, serial access, SDK/font changes, staging, commits, or pushes were performed.

## Evidence

- `src/activities/reader/PageTextSource.h:37`: the new owner holds value metadata only. Move assignment transfers the unique array, releases any previous destination array, empties the source metadata, and guards self-move. `clear()` releases storage. Existing horizontal source behavior is unchanged.
- `src/activities/reader/MangaPageTextSource.cpp:151`: the builder validates geometry/scope before publishing, clears stale output, bounds its allocation to 1024 × 16 bytes, and retries at 256 glyphs after a failed larger allocation. The existing scanner allocates at most one candidate per source glyph (`PageWordScanner.cpp:274`), so the corresponding candidate maximum is 8 KiB. No retained text copy, per-glyph allocation, or second framebuffer is introduced. Stack locals are compact; the allocation rationale is documented.
- `MangaPageTextSource.cpp:17` and `:51`: strict length-aware decoding and one common walker preserve lexical-run ordinals and UTF-8 byte offsets. Controls/NUL/malformed bytes break paragraphs; malformed bytes cannot shift the subsequent run's byte origin. The format caps a decoded page at 32768 bytes (`lib/MangaPanel/MangaFormat.h:11`), keeping these 16-bit paragraph/ordinal counters within range for valid format views.
- `MangaPageTextSource.cpp:196`: capacity and fallback-screen exhaustion publish only a completed Latin run or a complete Japanese codepoint prefix, with `truncated` set. The API explicitly requires visible truncation and forbids cache loading/saving for that source (`MangaPageTextSource.h:23`). Enforcement belongs to Task 9b under the controller's scope clarification.
- `MangaPageTextSource.cpp:112`: geometry uses the supplied full-page image fit, source clipping and 64-bit intermediates, enclosing floor/ceil scaling, relative orientation transforms, and base viewport clipping. Whole-block rectangles are shared by their glyphs; invisible rectangles remain empty. No panel crop origin or character subdivision is inferred. Text-only fixed-cell layout is explicit and bounded (`:207`).
- `MangaPageTextSource.cpp:263`: reconstruction validates both UTF-8 byte boundaries, matching source block and paragraph, range order and destination capacity before copying exact original bytes. Cross-ordinal whitespace is preserved. The run-relative offsets match the existing activity's backwards ordinal walk and UTF-8 length accumulation (`EpubReaderWordLookupActivity.cpp:918`). This remains valid after child teardown if the parent preserves its immutable page bytes and scope.
- `MangaPageTextSource.cpp:249` and `:296`: hashes include scope, codepoints and paragraph/run boundaries. Cache names use separate decimal physical-page/panel fields and a distinct overview suffix; their valid ranges cannot collide by integer packing. Geometry exclusion is appropriate for glyph-index scan candidates; partial sources must still bypass caches.

## Test assessment and limits

The 16 additions are actual adapter/backend tests in `test/japanese_dictionary/JapaneseDictionaryTest.cpp:6194`, linked to the existing dictionary executable by `test/japanese_dictionary/CMakeLists.txt:3`. The StarDict fixture runs the real engine/scanner against known and unknown whole words. The Japanese fixture runs the real backend with vocabulary, grammar and name files and compares longest/deinflected/grammar/name/counter segmentation to the existing plain-glyph fixture. Shared scanner and EPUB code are not altered by this patch.

The geometry test uses independent physical-coordinate and inverse-coordinate equations for all 16 image/base combinations, rather than reproducing the adapter's relative-turn loop. Additional cases exercise fractional enclosing scaling, asymmetric insets, source clipping, extreme rectangle inputs and invalid geometry. Reconstruction tests cover teardown, a cross-ordinal substring (`ello wor`), multibyte boundaries, controls, malformed data and capacity rejection. Allocation tests cover total failure, a failed large allocation followed by the bounded retry, long Japanese prefixes and discarded partial Latin suffixes.

The supplied native log `/private/tmp/crossink-manga-ocr9a-native.log` ends with **285 tests from 13 suites passed**; I inspected that recorded output and did not rerun tests. Tests do not execute the activity's actual return/teardown path, and the move test's `bytes.clear()` retains vector capacity. The implementation's value-only ownership and clipping-field compatibility are supported by source inspection; actual child activity lifetime and returned clipping integration still need Task 9b coverage. These are review limits, not identified adapter defects.

The separately reported internal-support changelog placeholder is understood to be consolidated by Task 9b. It does not establish a completed user-facing OCR feature.

## Task 9b / hardware verification boundary

Task 9b must retain the immutable parent page/scope, move the owner into the shared lookup activity, choose the full-page background or text fallback, reconstruct all four returned clipping fields, visibly mark truncation, reject partial scan caches, and restore prior page/panel/orientation. Verify on X3/X4 and an S3 target with overview and legacy-crop books under every orientation: whole-block highlights must match the displayed overview, fallback text must stay selectable within its area, Japanese substrings and Latin clips must reproduce exact source text, and Back must restore the prior reader state. Exercise >1024 glyphs and forced low-memory entry and confirm no partial cache reuse. No adapter-only EPUB/image cache reset is required.
