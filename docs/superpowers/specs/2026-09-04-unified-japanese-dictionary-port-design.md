# Unified Japanese Dictionary Port Design

Date: 2026-09-04  
Status: Approved  
Parity reference: local `../matcha-reader` at `61ca61ba86e3c5709a24d1b9c4f3cf2d41488012`

## Summary

CrossInk will gain Matcha Reader's Japanese dictionary capabilities while retaining its existing StarDict support. Both backends will be presented through one engine, one progressive page scanner, and one Matcha-style floating lookup activity. Japanese EPUBs will use Matcha-compatible vocabulary, names, and grammar indexes with longest-match segmentation and conjugation handling. Other languages will continue using CrossInk's current StarDict behavior.

The port must be safe on ESP32-C3 devices, preserve the current reader and dictionary features, and accept dictionary files generated for Matcha without conversion or migration. Dictionary parity is completed and verified before manga work begins.

## Goals

- Preserve CrossInk's current StarDict support for non-Japanese books and fallback lookup.
- Add Japanese-aware longest-match segmentation, deinflection, names lookup, grammar lookup, priority rules, and part-of-speech validation.
- Preserve exact compatibility with Matcha's preferred and legacy Japanese dictionary files.
- Use one backend-neutral dictionary API and one floating lookup UI for every language.
- Port Matcha's progressive page scanning, selectable-word navigation, cursor restoration, scan caching, ruby glossary, and low-memory recovery for horizontal EPUB pages.
- Preserve CrossInk features including HTML definitions, dictionary switching, lookup history, suggestions where supported, custom dictionary fonts, clipping creation, touch controls, buttons, orientation, and dark mode.
- Include the in-repository command-line converter and byte-compatibility tests.
- Leave an explicit page-source seam for later vertical-text and manga adapters.

## Non-goals

- Vertical Japanese layout or `VerticalPage` lookup. Only the adapter boundary is designed now.
- Manga reading, manga OCR, or manga lookup. Manga starts only after this dictionary parity gate passes.
- Page translation or network dictionary lookup.
- Integration with the separate Matcha Reader Tools or Inky browser applications.
- A new permanent user setting for choosing the backend.
- Replacing StarDict files with the Japanese format or exposing vocabulary, names, and grammar as three user-selected dictionaries.

## Scope and source boundaries

The Matcha implementation is a behavioral reference, not a tree-level replacement. The main source areas are:

- `../matcha-reader/lib/Dict/`: Japanese index, lookup, and deinflection behavior.
- `../matcha-reader/src/activities/reader/WordSelectionScan.*`: Japanese page segmentation and scan cache.
- `../matcha-reader/src/activities/reader/EpubReaderWordLookupActivity.*`: floating lookup interaction and rendering.
- `../matcha-reader/lib/Epub/Epub/RubyGlossary.*`: per-book ruby readings.
- `../matcha-reader/tools/dict_convert/convert_jmdict.py` and `scripts/gen_dict_spx.py`: compatible host-side output.

CrossInk remains authoritative for activity lifecycle, rendering synchronization, StarDict behavior, fonts, settings, translations, clipping, and current device support. Existing generated files are regenerated from their source data and are never edited directly.

## User-visible behavior

### Backend selection

1. The reader normalizes the EPUB language tag to a lowercase primary language subtag.
2. A primary language of `ja` selects the Japanese backend when its required vocabulary index and data files validate.
3. Optional names and grammar dictionaries are used when present and valid. A missing optional source does not disable vocabulary lookup.
4. If the required Japanese files are unavailable or invalid, lookup falls back to the language-selected or globally configured StarDict dictionary.
5. Non-Japanese books use the current language-selected StarDict behavior.
6. Reader Settings displays the effective backend or fallback dictionary. Backend selection is automatic and is not another persisted setting.

### Floating lookup

- Every language opens the same floating lookup panel over the current page.
- The selected word remains highlighted on the underlying page.
- Existing CrossInk entry gestures and shortcut mappings continue to open lookup.
- Previous/next navigation moves through selectable candidates and cycles only after the page scan is complete.
- A cold Japanese scan performs a bounded initial burst and displays the first available definition within 1.5 seconds. The remaining page scan continues incrementally while the user reads.
- Reopening the same unchanged page restores the scan and cursor from cache.
- Japanese results show the canonical headword, deinflected match, applicable reading, source-aware definition ordering, and book-provided ruby reading when available.
- Dictionary switching, lookup history, clipping, definition pagination, custom dictionary fonts, touch, button hints, dark mode, and orientation remain available where they are supported today.
- Backend-specific unsupported actions are omitted or disabled explicitly; they never fail silently.

## Architecture

### Dictionary engine

`DictionaryEngine` is the only dictionary API used by reader activities. It is an activity-owned value with explicit `open()`, `probe()`, `lookup()`, `suggest()`, `cancel()`, and `close()` lifecycle operations.

The engine exposes backend-neutral types:

- `DictionaryBackendKind`: `StarDict` or `Japanese`.
- `DictionaryCapabilities`: flags for suggestions, stem handling, Japanese deinflection, names, grammar, and ruby readings.
- `DictionaryQuery`: UTF-8 source text, byte offset, lookup mode, and whether definition content is required.
- `DictionaryResult`: status, matched source length, canonical headword, reading, source, deinflection/stem metadata, and a backend-owned definition reference.
- `DictionaryStatus`: `Found`, `NotFound`, `Unavailable`, `ReadError`, `Cancelled`, or `OutOfMemory`.

The UI does not open dictionary files or interpret backend-specific locations. It asks the engine to present a result through the existing bounded definition-rendering path. The backend may use an on-disk reference or one resident definition, but that choice stays behind the engine. Result handles remain valid until the next lookup or `close()`. Worker completion transfers a result into an activity-owned immutable slot; that slot is not replaced while the render task is consuming it.

Backend dispatch is enum-driven and uses static or activity-owned storage. It does not allocate a polymorphic backend object and does not use `std::function`.

### StarDict adapter

`StarDictBackend` wraps the existing `src/util/Dictionary.*` implementation rather than copying or rewriting it. It preserves:

- configured and per-book dictionary selection;
- `.idx`, `.dict`, `.ifo`, `.syn`, `.qidx`, `.oft`, and `.oft.cspt` handling;
- exact and stem-variant lookup;
- spelling suggestions and alternate forms;
- HTML definition rendering and bounded streaming;
- lookup history, switching, and clipping integration.

Space-delimited page scanning uses CrossInk's existing token boundaries. It does not run Japanese segmentation rules over non-Japanese text.

### Japanese adapter

`JapaneseBackend` ports Matcha's `DictIndex`, `WordLookup`, and `Deinflector` behavior. It provides:

- longest-first windows from eight Unicode characters down to one;
- raw lookup followed by chained deinflection candidates;
- part-of-speech validation for ichidan, godan, suru, kuru, and i-adjective candidates;
- vocabulary, names, and grammar source filtering;
- priority-based selection and merging of same-headword records;
- reading-record collision suppression;
- Matcha's particle, counter, kana, katakana-name, honorific, and grammar-priority handling;
- definition-free probes during page segmentation to avoid unnecessary `.dat` reads;
- explicit release of sparse indexes, block caches, and open files at session end.

### Page text source and scanner

`PageTextSource` is a small read-only interface that exposes codepoints, UTF-8 text ranges, paragraph identity, glyph rectangles, and font/style identity. `HorizontalPageTextSource` adapts the current EPUB `Page` without copying the framebuffer or keeping a second rendered page.

`PageWordScanner` owns compact glyph-reference and candidate buffers and uses a backend-selected strategy:

- StarDict strategy: existing word/token boundaries and lazy definition lookup.
- Japanese strategy: Matcha-compatible incremental longest-match probing and filtering.

The source is counted first, then the scanner allocates fallible fixed-capacity arrays once. It does not depend on exception-throwing `std::vector` growth. Only the main loop mutates scanner state. The render task consumes an immutable snapshot of the selected candidate. A later vertical-text project may add `VerticalPageTextSource` without changing the engine or floating activity.

### Floating lookup activity

The unified activity ports Matcha's floating panel appearance and navigation while integrating CrossInk services. It owns:

- the dictionary engine session;
- the page-source adapter and scanner;
- the current candidate and immutable render snapshot;
- the active definition presentation state;
- scan-cache and cursor state;
- interaction state for navigation, switching, history, clipping, and dismissal.

CrossInk's legacy word-selection and definition activities remain available during migration. They are removed only after all parity tests pass; the final firmware has one user-facing lookup flow.

### Registry and settings

`DictionaryRegistry` learns that the bare `/dictionaries/jp` folder belongs to the Japanese backend and must not be offered as a StarDict dictionary. Nested valid StarDict dictionaries remain discoverable.

The existing global and per-book StarDict selections remain fallback configuration. The existing dictionary font family and size settings apply to the unified floating panel. Japanese vocabulary, names, and grammar sources are a combined backend and are not separate selector entries.

## Compatible on-disk formats

### Japanese index and data

Preferred files:

- `/dictionaries/jp/vocab.idx`, `vocab.dat`, and optional `vocab.spx`;
- `/dictionaries/jp/names.idx`, `names.dat`, and optional `names.spx`;
- `/dictionaries/jp/grammar.idx`, `grammar.dat`, and optional `grammar.spx`.

Legacy Matcha paths and basenames under `/dictionaries/jp` and `/dict` remain supported.

Each `.idx` is a sorted array of packed 40-byte little-endian records:

- 32-byte NUL-padded UTF-8 headword;
- 32-bit `.dat` byte offset;
- 16-bit definition length;
- 8-bit priority;
- 8-bit part-of-speech flags.

The `.dat` file is the variable-length definition blob. Index validation rejects non-multiple record sizes, unterminated or invalid headwords, offsets outside `.dat`, and unsupported lengths before content is exposed to the UI.

The optional `.spx` sidecar remains Matcha version 1: an eight-byte `CPSPX1` magic, 32-byte header, stride 48, source record count, and 32-byte checkpoint keys. A missing or stale sidecar falls back to full binary search without changing lookup results.

### Ruby glossary

Horizontal EPUB parsing harvests bounded unique base/ruby pairs into `<book-cache>/ruby.bin` using Matcha's version-1 format. Corrupt or truncated glossary data is ignored as a whole. The serialized page fields do not change, but the horizontal section cache generation is advanced once so books cached before glossary support are reparsed and cannot silently retain an empty glossary. The invalidation and unchanged record layout are documented in `docs/file-formats.md`.

### Page scan cache

`<book-cache>/wlscan.bin` remains a disposable single-page cache. Its header identifies the scanner schema, spine, page, glyph-content hash, dictionary signature, candidate count, and last cursor. The unified implementation extends the identity with backend kind and a stronger dictionary signature so a StarDict/Japanese switch or same-sized dictionary replacement cannot reuse incompatible segmentation.

Cache output is written to a temporary sibling and atomically renamed. Incomplete, low-memory-truncated, stale, corrupt, or failed writes are never used and never block lookup.

## Data flow

1. `EpubReaderActivity` receives a lookup gesture and pauses reading-time accounting.
2. It loads or retains the current horizontal `Page`, creates the page adapter, and supplies language, book cache path, spine, page, font, and renderer context to the floating activity.
3. The registry resolves the effective backend. Japanese validation distinguishes required vocabulary files from optional names and grammar files.
4. The activity attempts to load `wlscan.bin`. A valid hit restores candidates and cursor.
5. On a miss, the scanner performs an initial burst of at most 1.5 seconds, composed of lookup slices no longer than 50 ms, until it finds a candidate or reaches the cap.
6. The activity looks up the selected candidate, publishes an immutable result snapshot, and requests the floating-panel render.
7. When no definition job is active, subsequent loop iterations continue scanning in bounded slices.
8. Navigation uses discovered candidates immediately. If the user reaches the temporary end, scanning advances on demand without falsely cycling to the first result.
9. On dismissal, the activity persists a complete scan and cursor, cancels and joins outstanding work, closes all files, releases backend caches, and returns control to the EPUB reader.

## Concurrency and storage ownership

- The existing single static 4 KB dictionary worker is retained and generalized for the unified engine.
- Definition retrieval runs on that worker so slow StarDict reads do not freeze input.
- Incremental scanning runs on the main loop only when the definition worker is idle. Scanner probes and definition reads therefore never race over shared backend file/cache state.
- The scanner's mutable vectors are main-loop-only. Rendering uses a small immutable selected-candidate/result snapshot.
- Font cache release, framebuffer restoration, and any reader-page reload occur under `RenderLock`.
- Activity destruction first requests cancellation, waits until the worker no longer owns the activity, and only then releases session state.
- All `FsFile`/`HalFile` handles are explicitly closed. The design never opens a second reader for the same file path.
- Activity stack changes use `ActivityManager` request/result APIs and occur outside held render locks.

## Memory model

- No second framebuffer or screen-sized snapshot is permitted.
- No heap-allocated backend polymorphism is permitted.
- The static dictionary task stack remains 4 KB and is reused across lookups.
- The Japanese sparse/block cache budget starts from Matcha's measured approximately 30 KB and is released on activity exit.
- The page source is counted before extraction. Candidate and glyph arrays are allocated once with `makeUniqueNoThrow<T[]>()`, capacity-checked on every append, and released with the activity. A failed full-capacity allocation may retry once with a smaller explicitly bounded capacity; truncation is reported and never cached.
- Temporary buffers larger than 256 bytes use fallible owned heap allocation, not task stack storage.
- Definition presentation retains at most the active result/page. It does not accumulate prior definitions.
- Large dictionary content remains on SD and is accessed through indexes and bounded reads.
- Font memory may be reclaimed only under `RenderLock`, and the reader rewarms fonts lazily after return.
- The implementation records total free heap, largest allocatable block, scanner high-water usage, cache allocation sizes, and worker stack high-water during hardware validation.

## Error handling

- Every allocation, file open, seek, read, parse, and cache write failure is logged before returning a recoverable status.
- Bare `new`/`new[]` and abort-on-OOM paths are not permitted.
- Missing Japanese vocabulary files select StarDict fallback when available; otherwise the existing no-dictionary message is shown.
- Missing names or grammar files reduce capabilities without failing vocabulary lookup.
- Invalid Japanese records or short reads produce `ReadError`, not `NotFound`.
- Cancellation is distinct from error and never displays stale failure UI.
- A low-memory scan may release eligible font caches and restart once. A second failure leaves reading functional, reports memory pressure, and does not persist partial candidates.
- Corrupt scan and ruby caches are discarded individually and rebuilt.
- A failed converter run leaves no apparently complete final output set.

## Converter

The repository gains a host-side converter based on Matcha's `convert_jmdict.py` plus `.spx` generation. It supports the Matcha inputs in scope: Yomitan/Yomichan ZIP, jmdict-simplified JSON or archive, and optional MDict when its dependency is installed.

The converter:

- emits preferred `vocab`, `names`, or `grammar` basenames;
- preserves record ordering, headword limits, priority, POS flags, definition deduplication, and truncation behavior;
- produces version-1 `.spx` sidecars with Matcha's exact header and stride;
- validates all outputs before atomic publication;
- reports skipped or truncated entries;
- can run without network access when an input file is supplied.

The external browser converter is explicitly deferred.

## Migration sequence

1. Capture Matcha and CrossInk behavior, file hashes, UI screenshots, and C3 heap baselines using shared fixtures.
2. Add converter compatibility fixtures and Japanese record/sidecar readers with host tests.
3. Port Japanese deinflection and longest-match behavior with table-driven tests.
4. Introduce the unified engine and StarDict adapter while leaving existing activities as the active path.
5. Add language routing, fallback behavior, and effective-backend settings display.
6. Add horizontal page adaptation, backend-specific scanning strategies, and the versioned scan cache.
7. Add horizontal ruby harvesting and glossary lookup.
8. Port the floating activity and integrate CrossInk definition rendering, fonts, switching, history, suggestions, clipping, touch, buttons, orientation, and dark mode.
9. Switch normal lookup entry points to the unified activity behind an internal development gate.
10. Run parity, simulator, build, and hardware gates. Fix discrepancies rather than preserving both behaviors indefinitely.
11. Remove the internal gate and legacy lookup activities only after parity passes.
12. Update user documentation, developer cache-format documentation, and `CHANGELOG.md`.

## Verification strategy

### Host tests

- Byte-for-byte converter comparison against Matcha for small checked-in Yomitan, JMdict, names, and grammar fixtures.
- Valid and invalid 40-byte records, offset/length boundaries, sorting, duplicate headwords, UTF-8 limits, and stale `.spx` handling.
- Deinflection tables for ichidan, godan subclasses, suru, kuru, i-adjectives, polite/negative/past/te/passive/causative/potential forms, chained rules, cycles, deduplication, and POS rejection.
- Longest-match cases for kanji, kana, mixed script, punctuation, particles, counters, full-width digits, long vowels, katakana names, honorifics, uncommon reading collisions, and grammar priority.
- Backend routing for Japanese tag variants, non-Japanese tags, no language, missing/corrupt Japanese files, optional-source absence, and StarDict fallback.
- StarDict regression coverage for exact/stem/synonym/suggestion paths and HTML definitions.
- Scan cache identity, replacement, corruption, truncation, cursor restoration, incomplete scan rejection, and atomic publication.
- Ruby glossary collection, deduplication, size caps, corruption, and multiple readings.

### Simulator and UI tests

- All existing lookup entry gestures and menu actions.
- Initial progressive result, continued background scan, on-demand advance, completion cycling, and no-result pages.
- Highlight geometry and restoration after panel redraw.
- Floating definition pagination, loading, not-found, read-error, cancellation, and memory-error states.
- StarDict and Japanese rendering, dictionary switching, history, suggestions, clipping, custom font and size, touch, button-only navigation, every supported orientation, and dark mode.
- Leaving lookup during an active job, leaving the reader, sleep/resume, manual refresh, and opening another book.

### Builds and static checks

- `pio run -e simulator`
- `pio run -e default`
- `pio run -e sticky`
- `pio run -e x4-pro`
- Relevant native CMake/CTest targets.
- `pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high`
- Firmware-size and memory-profile scripts before and after the port.

### Hardware verification

The primary memory gate is an ESP32-C3 X3 or X4 using a Japanese EPUB, Matcha-compatible vocabulary/names/grammar dictionaries, and an SD-card reader font. S3 touch behavior is verified separately on Sticky or X4 Pro.

- Cold Japanese lookup displays the first valid result within the 1.5-second initial cap.
- Individual scan slices do not exceed 50 ms.
- The same representative page produces equivalent candidates, headwords, readings, deinflection, source priority, and definitions on Matcha and CrossInk.
- Twenty lookup open/navigate/close cycles show no monotonic decline in largest allocatable block.
- After the soak, page turns, EPUB repagination, another lookup, sleep/resume, and opening a second book still succeed.
- Missing, corrupt, or replaced dictionary files produce the specified fallback or error behavior without restart or abort.
- Touch targets and button mappings work on the concrete device profile being tested.

## Completion criteria

Dictionary parity is complete only when:

- all preferred and legacy Matcha dictionary fixtures load unchanged;
- converter golden outputs match Matcha byte for byte;
- the Japanese semantic fixture suite matches Matcha results;
- the floating panel serves both backends and retains every existing CrossInk dictionary capability applicable to that backend;
- all required builds, host tests, simulator flows, static checks, and C3/S3 hardware gates pass;
- there is no permanent compatibility flag or duplicate user-facing lookup flow;
- cache formats and setup instructions are documented;
- `CHANGELOG.md` contains a user-facing Added entry;
- the port does not edit generated outputs directly, and all unrelated user changes remain untouched.

Only after these criteria pass does manga design or implementation begin.
