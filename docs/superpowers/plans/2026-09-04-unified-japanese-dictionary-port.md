# Unified Japanese Dictionary Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give CrossInk full Matcha-compatible Japanese dictionary lookup—converter, indexes, longest-match segmentation, deinflection, vocabulary/names/grammar sources, ruby readings, scan caching, and one floating lookup UI—without regressing StarDict or ESP32-C3 stability.

**Architecture:** Reader activities call one value-owned `DictionaryEngine`. It dispatches through an enum to a thin StarDict adapter or an activity-owned Japanese backend, while a fixed-capacity page scanner and a generalized single 4 KB worker keep SD work bounded. A horizontal `PageTextSource` feeds one floating activity; vertical and manga adapters remain deferred until dictionary parity passes.

**Tech Stack:** C++20/Arduino-ESP32, FreeRTOS, SdFat through `HalStorage`, PlatformIO, native CMake/GoogleTest, Python 3 `unittest`, FreeInkUI, existing `DictHtmlRenderer` and `DictLayout`.

**Spec:** [Approved unified Japanese dictionary design](../specs/2026-09-04-unified-japanese-dictionary-port-design.md)

## Global Constraints

- Never run `git commit`, `git push`, or stage files. Every task ends with tests plus `git diff --check` and `git status --short` instead of a commit.
- Preserve the user's unrelated untracked `.codegraph/` directory and any later unrelated changes.
- Use CodeGraph before broad source searches while `.codegraph/` exists.
- Treat local `../matcha-reader` commit `61ca61ba86e3c5709a24d1b9c4f3cf2d41488012` as the parity reference. Port behavior, not whole directories.
- Keep the default ESP32-C3 target within its internal-RAM budget: no second framebuffer, no bare `new`, no growing `std::vector` in scan/deinflection hot paths, no buffer over 256 bytes on a task stack, and no PSRAM assumption.
- Use `makeUniqueNoThrow<T[]>()` for fallible owned arrays. Log allocation, file, parse, seek, read, and write failures before returning a recoverable status.
- App code uses `HalStorage`/`HalFile` (`FsFile` where the surrounding API already exposes it), not Arduino `File` or SDK storage classes. Close every opened handle explicitly.
- Do not edit generated I18n headers/sources by hand. Change `lib/I18n/translations/english.yaml` and regenerate with `scripts/gen_i18n.py`.
- Horizontal EPUB lookup is the only page adapter in this plan. Vertical Japanese, manga, OCR, manga caching, and manga thumbnails remain out of scope until every completion gate below passes.
- A partial or low-memory scan is usable for the current session but is never written to `wlscan.bin`.

## Baseline and source map

Current CrossInk seams to preserve:

- `src/activities/reader/EpubReaderActivity.cpp:3259` checks availability; `EpubReaderActivity.cpp:3305` builds the current page lookup flow; call sites also exist at lines 3497 and 4332.
- `src/activities/reader/DictionaryWordSelectActivity.cpp:586` enumerates horizontal `PageLine`/`TextBlock` data and owns current highlight/clipping behavior.
- `src/activities/reader/DictionaryDefinitionActivity.cpp:518` through line 984 owns modal geometry, font fallback, HTML/plain streaming, paging, and definition word selection.
- `src/util/DictionaryLookupController.cpp:67` and line 570 own current StarDict job state and worker lookup.
- `src/util/DictionaryLookupWorker.h:13` is the reusable static 4 KB worker to generalize.
- `src/util/Dictionary.h:51` through line 168 is the StarDict API and on-disk definition reference; it remains authoritative.
- `src/util/DictionaryRegistry.cpp:16` discovers StarDict roots; its current single-level scan must learn the reserved `jp` bundle and nested language folders.
- `lib/Epub/Epub/Section.cpp:18` has section cache version 61 and partial version `0xF8`; ruby harvesting requires one semantic invalidation.
- `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h:300` is the horizontal parser constructor; it already parses/render ruby and only needs bounded glossary harvesting.

Matcha parity sources:

- `../matcha-reader/lib/Dict/DictIndex.{h,cpp}`: 40-byte records, preferred/legacy paths, sparse index, priority/POS/source behavior, and cache release.
- `../matcha-reader/lib/Dict/Deinflector.cpp:26` and line 390: ordered 260-rule table and 64-candidate chained traversal.
- `../matcha-reader/lib/Dict/WordLookup.{h,cpp}`: longest-first eight-codepoint lookup and deinflection validation.
- `../matcha-reader/src/activities/reader/WordSelectionScan.cpp:222`: horizontal flattening; line 387: scan-cache behavior.
- `../matcha-reader/src/activities/reader/EpubReaderWordLookupActivity.cpp:84`: cache/initial burst; lines 97–98: 1.5-second cap and 50 ms slices.
- `../matcha-reader/lib/Epub/Epub/RubyGlossary.cpp:15`: version-1 glossary; lines 18–25: 32-byte strings, 200 pairs/section, 1,024 records, and 16 KB file cap.
- `../matcha-reader/tools/dict_convert/convert_jmdict.py` and `../matcha-reader/scripts/gen_dict_spx.py`: host format reference.

## Planned file responsibilities

| Path | Responsibility |
| --- | --- |
| `tools/dict_convert/convert_jmdict.py` | Offline Yomitan/JMdict/MDict conversion and atomic publication. |
| `scripts/gen_dict_spx.py` | Matcha version-1 `.spx` generation. |
| `lib/Dict/DictIndex.{h,cpp}` | Validated indexed Japanese file access and bounded sparse/block caches. |
| `lib/Dict/Deinflector.{h,cpp}` | Allocation-free ordered Japanese conjugation expansion. |
| `lib/Dict/WordLookup.{h,cpp}` | Longest-match Japanese probe/full lookup semantics. |
| `src/util/DictionaryEngineTypes.h` | Backend-neutral requests, capabilities, statuses, results, handles, and definition spans. |
| `src/util/StarDictBackend.{h,cpp}` | Thin adapter over existing `Dictionary` static APIs. |
| `src/util/JapaneseDictionaryBackend.{h,cpp}` | Adapter over the instance-owned `lib/Dict` session. |
| `src/util/DictionaryEngine.{h,cpp}` | Lifecycle, enum dispatch, fallback, result generation, definition streaming, and signatures. |
| `src/activities/reader/PageTextSource.{h,cpp}` | Fixed-capacity horizontal `Page` to glyph/source adapter; future vertical/manga seam. |
| `src/activities/reader/PageWordScanner.{h,cpp}` | Backend-selected progressive segmentation and navigation candidates. |
| `src/activities/reader/PageWordScanCache.{h,cpp}` | Versioned `wlscan.bin` validation and recoverable atomic replacement. |
| `lib/Epub/Epub/RubyGlossary.{h,cpp}` | Matcha-compatible `ruby.bin` collect/merge/lookup. |
| `src/activities/reader/DictionaryDefinitionModel.{h,cpp}` | Worker-built, backend-neutral, current-page definition layout snapshot. |
| `src/activities/reader/DictionaryLookupFlow.{h,cpp}` | Pure state transitions for progressive navigation/loading/errors. |
| `src/activities/reader/EpubReaderWordLookupActivity.{h,cpp}` | Unified floating panel for page and history lookups. |
| `test/japanese_dict_converter/` | Python converter and golden-byte tests. |
| `test/japanese_dictionary/` | Native index, deinflection, lookup, engine, ruby, scanner, cache, and state tests. |

---

### Task 1: Freeze converter compatibility and parity fixtures

**Files:**

- Create: `test/japanese_dict_converter/__init__.py`
- Create: `test/japanese_dict_converter/test_converter.py`
- Create: `test/japanese_dict_converter/fixtures/mini_jmdict.json`
- Create: `test/japanese_dict_converter/fixtures/mini_yomitan/index.json`
- Create: `test/japanese_dict_converter/fixtures/mini_yomitan/term_bank_1.json`
- Create: `test/japanese_dict_converter/golden/README.md`
- Create: `test/japanese_dictionary/fixtures/parity_cases.json`

**Interfaces:** Produces deterministic source fixtures and checked expected byte hashes consumed by Tasks 2–4. Consumes Matcha converter behavior at the pinned commit; no firmware interface yet.

- [ ] Verify the reference before capturing data:

  ```bash
  git -C ../matcha-reader rev-parse HEAD
  python3 ../matcha-reader/tools/dict_convert/convert_jmdict.py --help
  ```

  The first command must print `61ca61ba86e3c5709a24d1b9c4f3cf2d41488012`. If it does not, record the actual SHA in the test fixture README and stop this task for review; do not silently refresh goldens.

- [ ] Add minimal fixtures containing: one ichidan entry (`食べる`), one godan entry (`書く`), a kana reading collision, duplicate headwords with different priorities, one proper name, one grammar expression, structured Yomitan content, and a headword whose UTF-8 encoding is exactly at the 31-byte accepted limit.

- [ ] Write the test first. It must import the future CrossInk converter and assert the packed layout rather than merely snapshotting a file:

  ```python
  RECORD = struct.Struct("<32sIHBB")

  def test_record_layout_and_matcha_golden(self):
      self.assertEqual(RECORD.size, 40)
      self.assertEqual(self.outputs["vocab.idx"], self.golden["vocab.idx"])
      self.assertEqual(self.outputs["vocab.dat"], self.golden["vocab.dat"])
      self.assertEqual(self.outputs["vocab.spx"], self.golden["vocab.spx"])
      headword, offset, length, priority, pos_flags = RECORD.unpack_from(
          self.outputs["vocab.idx"], 0
      )
      self.assertIn(b"\0", headword)
      self.assertLessEqual(offset + length, len(self.outputs["vocab.dat"]))
      self.assertGreater(priority, 0)
      self.assertNotEqual(pos_flags, 0)
  ```

- [ ] Run the new test and confirm the expected import/file failure:

  ```bash
  python3 -m unittest test.japanese_dict_converter.test_converter -v
  ```

- [ ] Use the pinned Matcha scripts in a temporary directory to generate the checked golden bytes and SHA-256 values. Store small binary goldens as files under `golden/`; document the exact reference command and SHA in `golden/README.md`.

- [ ] Add `parity_cases.json` with explicit expected surface, canonical headword, matched UTF-8 byte count, source mask, deinflected flag, and priority for at least: `食べました`, `書かなかった`, `勉強している`, `来られた`, an i-adjective past form, a katakana name plus honorific, a trailing case particle, a counter, and a grammar phrase.

- [ ] Capture the pre-port baseline before touching firmware code: build `pio run -e simulator` and `pio run -e default`, save Japanese/StarDict lookup screenshots from the existing flow, and record `ESP.getFreeHeap()`, `ESP.getMaxAllocHeap()`, firmware size, and current dictionary-worker high-water output for the same C3 book/page/font fixture used by Task 16.

- [ ] Re-run the test; it should still fail only because the CrossInk converter is absent.

- [ ] Review checkpoint:

  ```bash
  git diff --check
  git status --short
  ```

### Task 2: Port the host converter and `.spx` generator

**Files:**

- Create: `tools/dict_convert/convert_jmdict.py`
- Create: `scripts/gen_dict_spx.py`
- Modify: `test/japanese_dict_converter/test_converter.py`

**Interfaces:** Consumes local JMdict JSON/archive, Yomitan ZIP/directory, or optional MDict input. Produces atomically published `{vocab,names,grammar}.{idx,dat,spx}` with 40-byte little-endian records and Matcha version-1 `.spx` bytes.

- [ ] Port the reference CLI and its complete structured-content, priority, POS, reading-record, and source-name behavior with `apply_patch`. Preserve these format constants exactly:

  ```python
  RECORD = struct.Struct("<32sIHBB")
  HEADWORD_SIZE = 32
  SPX_MAGIC = b"CPSPX1\0\0"
  SPX_VERSION = 1
  SPX_HEADER_SIZE = 32
  SPX_STRIDE = 48
  ```

- [ ] Make each output set transactional: write `.idx.tmp`, `.dat.tmp`, and `.spx.tmp`; validate record sorting, NUL padding, record-size divisibility, offsets/lengths, header fields, and checkpoint keys; only then replace the final sibling files with `os.replace`. On failure, remove only the temp siblings created by that invocation.

- [ ] Keep the converter offline when an input is supplied. Import `readmdict` only inside the MDict branch and produce one actionable dependency error when it is absent.

- [ ] Extend tests for all three basenames, a corrupt source, a definition longer than `UINT16_MAX`, duplicate definition reuse, 32-byte headword rejection, stale temp files, and failed publication preserving a previous valid output set.

- [ ] Run converter tests:

  ```bash
  python3 -m unittest discover -s test/japanese_dict_converter -p 'test_*.py' -v
  ```

  Expected: every generated `.idx`, `.dat`, and `.spx` byte equals the pinned Matcha golden, including record order and POS flags.

- [ ] Run both CLIs against a temp copy of the fixture and inspect their summaries:

  ```bash
  python3 tools/dict_convert/convert_jmdict.py --input test/japanese_dict_converter/fixtures/mini_jmdict.json --output-dir /tmp/crossink-jp-dict --name vocab
  python3 scripts/gen_dict_spx.py /tmp/crossink-jp-dict
  ```

- [ ] Review checkpoint:

  ```bash
  git diff --check
  git status --short
  ```

### Task 3: Add the validated Japanese index reader

**Files:**

- Create: `lib/Dict/DictIndex.h`
- Create: `lib/Dict/DictIndex.cpp`
- Create: `test/japanese_dictionary/CMakeLists.txt`
- Create: `test/japanese_dictionary/JapaneseDictionaryTest.cpp`
- Create: `test/japanese_dictionary/stubs/Arduino.h`
- Create: `test/japanese_dictionary/stubs/HalStorage.h`
- Create: `test/japanese_dictionary/stubs/Logging.h`
- Create: `test/japanese_dictionary/stubs/Memory.h`
- Modify: `test/CMakeLists.txt`

**Interfaces:** Produces an instance-owned `DictIndex` session for `JapaneseDictionaryBackend`. Consumes exact converter files and `HalStorage`; returns explicit Japanese statuses without exposing file handles.

- [ ] Add the test target first and register it with `add_subdirectory(japanese_dictionary)`. The fake `HalStorage` maps firmware paths below a configurable temporary root and implements only `exists`, `openFileForRead`, `HalFile::isOpen/close/size/seek/read`.

- [ ] Write failing tests for preferred and every legacy path, optional names/grammar absence, stale/missing `.spx`, duplicate-headword merging, priority ordering, reading collision flags, record-size remainder, missing NUL, invalid UTF-8, out-of-range data slice, short read, and explicit close/reopen.

- [ ] Define the production API exactly as follows:

  ```cpp
  enum class JapaneseDictStatus : uint8_t {
    Found,
    NotFound,
    Unavailable,
    ReadError,
    OutOfMemory,
  };

  struct DictIndexRecord {
    static constexpr size_t HEADWORD_SIZE = 32;
    char headword[HEADWORD_SIZE];
    uint32_t offset;
    uint16_t length;
    uint8_t priority;
    uint8_t posFlags;
  } __attribute__((packed));
  static_assert(sizeof(DictIndexRecord) == 40);

  struct DictProbe {
    char headword[DictIndexRecord::HEADWORD_SIZE] = {};
    uint8_t priority = 0;
    uint8_t sourceDict = 0;
    uint8_t posFlags = 0;
  };

  class DictIndex {
   public:
    JapaneseDictStatus open();
    JapaneseDictStatus probeExact(std::string_view headword, DictProbe& out,
                                  uint8_t dictMask = DICT_ALL, uint8_t posMask = 0);
    JapaneseDictStatus lookupExact(std::string_view headword, DictEntry& out,
                                   uint8_t dictMask = DICT_ALL, uint8_t posMask = 0);
    uint8_t availableSources() const;
    uint64_t signature() const;
    void close();
  };
  ```

- [ ] Port Matcha's preferred and legacy resolution order, `DICT_JMDICT=1`, `DICT_GRAMMAR=2`, `DICT_NAMES=4`, source filtering, merge ordering, POS checks, sparse coarse/fine index, 64-record block cache, and approximately 30 KB maximum cache shape into instance members.

- [ ] Decode `offset` and `length` from a 40-byte read with `memcpy`; never cast an unaligned byte pointer. Validate the record headword and its `.dat` bounds before returning `Found`. A malformed/short record returns `ReadError`, not `NotFound`.

- [ ] Compute the stable 64-bit signature as FNV-1a over the resolved path tag, index byte size, first up-to-256 bytes, and last up-to-256 bytes for each available source. This catches a same-sized replacement without reading entire dictionaries.

- [ ] Allocate coarse, fine, and block caches with `makeUniqueNoThrow`; failure disables only that accelerator and falls back to direct binary search. Close only handles whose `isOpen()` is true.

- [ ] Configure and run the native test:

  ```bash
  cmake -S test -B build/tests
  cmake --build build/tests --target JapaneseDictionaryTest -j2
  ctest --test-dir build/tests --output-on-failure -R JapaneseDictionary
  ```

- [ ] Review checkpoint:

  ```bash
  git diff --check
  git status --short
  ```

### Task 4: Port allocation-free deinflection and longest-match lookup

**Files:**

- Create: `lib/Dict/Deinflector.h`
- Create: `lib/Dict/Deinflector.cpp`
- Create: `lib/Dict/WordLookup.h`
- Create: `lib/Dict/WordLookup.cpp`
- Modify: `test/japanese_dictionary/JapaneseDictionaryTest.cpp`
- Modify: `test/japanese_dictionary/CMakeLists.txt`

**Interfaces:** Consumes an opened `DictIndex` and UTF-8 text/offset. Produces fixed-size probes or one full `DictEntry`; rule order and first-match behavior match Matcha.

- [ ] Write failing table-driven tests from `parity_cases.json`, plus all verb classes, polite/negative/past/te/passive/causative/potential chains, duplicate suppression, 64-candidate cap, invalid UTF-8, non-boundary offsets, and POS rejection.

- [ ] Replace Matcha's hot-path `std::vector<std::string>` with the exact fixed contract below. The converter cannot emit a headword of 32 bytes, so candidates of 32 bytes or more can be skipped without losing a possible hit:

  ```cpp
  struct DeinflectionCandidate {
    char text[DictIndexRecord::HEADWORD_SIZE] = {};
    uint8_t byteLength = 0;
    WordCondition condition = WordCondition::DICT;
  };

  struct DeinflectionBuffer {
    static constexpr uint8_t kCapacity = 64;
    std::array<DeinflectionCandidate, kCapacity> candidates{};
    uint8_t count = 0;
  };

  class Deinflector {
   public:
    static void deinflect(std::string_view surface, DeinflectionBuffer& out);
  };
  ```

- [ ] Port all 260 rules in their original order. Preserve Matcha's breadth-first traversal, condition propagation, first duplicate wins, and hard 64-candidate stop. Use `static constexpr` rule data so it remains in flash.

- [ ] Define `WordLookup` as an instance borrowing `DictIndex`:

  ```cpp
  struct WordLookupProbe {
    size_t matchLength = 0;
    bool deinflected = false;
    uint8_t sourceDict = 0;
    uint8_t priority = 0;
  };

  class WordLookup {
   public:
    static constexpr uint8_t MAX_WINDOW_CHARS = 8;
    explicit WordLookup(DictIndex& index) : index_(index) {}
    JapaneseDictStatus probe(std::string_view text, size_t byteOffset, WordLookupProbe& out);
    JapaneseDictStatus lookup(std::string_view text, size_t byteOffset, WordLookupResult& out);
  };
  ```

- [ ] Port longest-first raw lookup, then ordered deinflection, source narrowing, POS-mask validation, same-headword merges, reading-record suppression, particle/counter rules, kana/CJK classification, katakana name plus honorific grouping, and grammar priority.

- [ ] Ensure `probe()` never reads `.dat` and does not allocate. `lookup()` may allocate only the active headword/reading/definition and runs on the dictionary worker.

- [ ] Run:

  ```bash
  cmake --build build/tests --target JapaneseDictionaryTest -j2
  ctest --test-dir build/tests --output-on-failure -R JapaneseDictionary
  ```

  Expected: every semantic result in `parity_cases.json` matches the pinned Matcha result, including matched UTF-8 byte length and source priority.

- [ ] Review checkpoint:

  ```bash
  git diff --check
  git status --short
  ```

### Task 5: Introduce the unified engine and both backend adapters

**Files:**

- Create: `src/util/DictionaryEngineTypes.h`
- Create: `src/util/StarDictBackend.h`
- Create: `src/util/StarDictBackend.cpp`
- Create: `src/util/JapaneseDictionaryBackend.h`
- Create: `src/util/JapaneseDictionaryBackend.cpp`
- Create: `src/util/DictionaryEngine.h`
- Create: `src/util/DictionaryEngine.cpp`
- Modify: `test/japanese_dictionary/JapaneseDictionaryTest.cpp`
- Modify: `test/japanese_dictionary/CMakeLists.txt`

**Interfaces:** `DictionaryEngine` is the sole final reader-facing dictionary API. It consumes book language/cache context and emits backend-neutral probes/results/definition spans. StarDict continues to consume current `Dictionary`; Japanese consumes `DictIndex`/`WordLookup`.

- [ ] Write failing engine tests with small function-pointer backend fakes: Japanese selected for normalized `ja`, `JA-jp`, and `ja_JP`; valid Japanese vocabulary wins; unavailable/corrupt Japanese falls back to StarDict; missing optional sources only clear capability bits; non-Japanese/no-language use StarDict; cancellation/read errors/OOM remain distinct.

- [ ] Add the backend-neutral types:

  ```cpp
  enum class DictionaryBackendKind : uint8_t { StarDict, Japanese };
  enum class DictionaryStatus : uint8_t {
    Found, NotFound, Unavailable, ReadError, Cancelled, OutOfMemory
  };
  enum class DictionaryLookupMode : uint8_t { Token, LongestAtOffset };

  struct DictionaryCapabilities {
    bool suggestions = false;
    bool stemVariants = false;
    bool deinflection = false;
    bool names = false;
    bool grammar = false;
    bool ruby = false;
  };

  struct DictionaryQuery {
    std::string_view text;
    size_t byteOffset = 0;
    DictionaryLookupMode mode = DictionaryLookupMode::Token;
  };

  struct DictionaryProbeResult {
    DictionaryStatus status = DictionaryStatus::NotFound;
    size_t matchedBytes = 0;
    bool transformed = false;
    uint8_t sourceMask = 0;
  };

  struct DictionaryDefinitionHandle { uint32_t generation = 0; };

  struct DictionarySuggestions {
    static constexpr uint8_t kCapacity = 8;
    std::array<std::string, kCapacity> items{};
    uint8_t count = 0;
  };

  struct DictionaryResult {
    DictionaryStatus status = DictionaryStatus::NotFound;
    DictionaryBackendKind backend = DictionaryBackendKind::StarDict;
    size_t matchedBytes = 0;
    std::string surface;
    std::string headword;
    std::string reading;
    bool transformed = false;
    uint8_t sourceMask = 0;
    DictionaryDefinitionHandle definition;
  };
  ```

- [ ] Add the exact C-style definition stream contract below. Do not use `std::function`:

  ```cpp
  enum class DictionaryDefinitionMode : uint8_t { Styled, PlainFallback };

  struct DictionaryDefinitionSpan {
    std::string_view text;
    bool bold = false;
    bool italic = false;
    bool superscript = false;
    bool subscript = false;
    bool ipa = false;
    bool listItem = false;
    bool lineBreak = false;
    uint8_t indentLevel = 0;
  };

  struct DictionaryDefinitionSink {
    void* context = nullptr;
    bool (*onSpan)(void*, const DictionaryDefinitionSpan&) = nullptr;
  };
  ```

- [ ] Implement the lifecycle:

  ```cpp
  struct DictionaryOpenRequest {
    std::string_view bookLanguage;
    const char* bookCachePath = nullptr;
  };

  class DictionaryEngine {
   public:
    DictionaryStatus open(const DictionaryOpenRequest& request);
    DictionaryStatus probe(const DictionaryQuery& query, DictionaryProbeResult& out);
    DictionaryStatus lookup(const DictionaryQuery& query, DictionaryResult& out);
    DictionaryStatus suggest(std::string_view word, DictionarySuggestions& out);
    DictionaryStatus streamDefinition(DictionaryDefinitionHandle handle,
                                      DictionaryDefinitionMode mode,
                                      DictionaryDefinitionSink sink);
    DictionaryBackendKind backendKind() const;
    DictionaryCapabilities capabilities() const;
    uint64_t signature() const;
    void cancel();
    void close();
  };
  ```

- [ ] Implement dispatch with a `switch (backendKind_)` and concrete value members; do not allocate a backend object. `probe()` must not change the active definition generation. `lookup()` increments the generation only after publishing a complete successful result. `close()` invalidates handles, clears StarDict overrides, closes Japanese files, and releases caches.

- [ ] In `StarDictBackend`, map `Dictionary::locateWithStemVariants`, alternate forms, `findSimilar`, `readInfo`, and `resolveDefinitionSlice` without changing `Dictionary.*`. Adapt `DictHtmlRenderer` output to the neutral span sink; stream plain `.dict` slices in a reusable 512-byte heap buffer so no 512-byte worker-stack array is introduced.

- [ ] In `JapaneseDictionaryBackend`, map `WordLookup::probe/lookup`, expose vocabulary/names/grammar capability bits, and emit the active Japanese definition as neutral plain spans. Add ruby later without changing the result ABI.

- [ ] Add StarDict regression tests for direct/stem/alternate/suggestion capability routing and HTML/plain span mapping. Use backend fakes for state-machine tests and the existing StarDict fixture shape for adapter tests; do not rewrite StarDict lookup internals.

- [ ] Run:

  ```bash
  cmake --build build/tests --target JapaneseDictionaryTest -j2
  ctest --test-dir build/tests --output-on-failure -R JapaneseDictionary
  ```

- [ ] Review checkpoint:

  ```bash
  git diff --check
  git status --short
  ```

### Task 6: Route Japanese and language-specific StarDict safely

**Files:**

- Modify: `src/util/DictionaryRegistry.h`
- Modify: `src/util/DictionaryRegistry.cpp`
- Modify: `src/SettingsList.h`
- Modify: `lib/I18n/translations/english.yaml`
- Modify: `test/japanese_dictionary/JapaneseDictionaryTest.cpp`
- Modify: `test/japanese_dictionary/CMakeLists.txt`

**Interfaces:** Registry produces a Japanese-bundle availability summary plus an effective StarDict fallback path. Engine consumes both. Settings displays the effective backend but persists only current StarDict fallback choices.

- [ ] Write failing registry tests for both roots, case-insensitive roots, bare `/dictionaries/jp`, nested `/dictionaries/jp/<name>`, `ja→jp`, two-letter language folders, ambiguous roots, invalid nested traversal, and current global/per-book fallback.

- [ ] Extend `DictionaryRegistry` without replacing its existing persistence contract:

  ```cpp
  struct JapaneseDictionaryBundle {
    bool vocabulary = false;
    bool names = false;
    bool grammar = false;
  };

  JapaneseDictionaryBundle japaneseBundle() const;
  const DictionaryEntry* firstForLanguage(std::string_view language) const;
  bool resolveEffectiveStarDict(std::string_view language,
                                const char* bookCachePath,
                                std::string& basePathOut) const;
  ```

- [ ] During discovery, reserve the bare `jp` folder for Japanese index files and do not log it as an ambiguous StarDict dictionary. Descend exactly one additional level so `jp/<name>` remains discoverable. Reject backslashes, `..`, empty path segments, and deeper nesting.

- [ ] Normalize the primary language to lowercase ASCII; map `ja` to `jp`; otherwise use the two-letter primary subtag. Prefer the first sorted language-folder StarDict, then current per-book/global `Dictionary::readConfiguredDictPath(bookCachePath)`.

- [ ] Change `buildDictionarySetting` to accept `bookLanguage` and `showAppliedDictionary`. Global settings remain editable fallback selection; reader settings show `Japanese (vocabulary + optional names/grammar)` or the effective StarDict and do not create a new persisted backend setting.

- [ ] Add only English source strings now; Task 14 regenerates all I18n outputs:

  ```yaml
  STR_DICT_EFFECTIVE_JAPANESE: "Japanese (automatic)"
  STR_DICT_FALLBACK: "Fallback dictionary"
  ```

- [ ] Run native tests and a simulator compile to catch settings signature callers:

  ```bash
  cmake --build build/tests --target JapaneseDictionaryTest -j2
  ctest --test-dir build/tests --output-on-failure -R JapaneseDictionary
  pio run -e simulator
  ```

- [ ] Review checkpoint:

  ```bash
  git diff --check
  git status --short
  ```

### Task 7: Generalize the single static dictionary worker

**Files:**

- Modify: `src/util/DictionaryLookupWorker.h`
- Modify: `src/util/DictionaryLookupWorker.cpp`
- Modify: `src/util/DictionaryLookupController.h`
- Modify: `src/util/DictionaryLookupController.cpp`
- Create: `test/japanese_dictionary/stubs/freertos/FreeRTOS.h`
- Create: `test/japanese_dictionary/stubs/freertos/task.h`
- Modify: `test/japanese_dictionary/JapaneseDictionaryTest.cpp`
- Modify: `test/japanese_dictionary/CMakeLists.txt`

**Interfaces:** Worker consumes `{owner, run}` without knowing controller type. Existing controller and the new floating activity both produce jobs. Ownership remains single-slot and callers cancel then wait before destruction.

- [ ] Write failing host tests for first start, busy rejection without overwriting the active callback, callback execution, owner release, wait semantics, and reuse by a second owner.

- [ ] Replace the controller-specific API with:

  ```cpp
  struct DictionaryWorkerJob {
    void* owner = nullptr;
    void (*run)(void*) = nullptr;
  };

  bool start(DictionaryWorkerJob job);
  bool isBusy() const;
  bool owns(const void* owner) const;
  void waitForOwner(const void* owner);
  ```

- [ ] Keep `kStackBytes = 4096` and `xTaskCreateStatic`. Claim `owner_` with compare-exchange before storing the callback, publish the callback before `xTaskNotify`, and clear `owner_` with release ordering only after the callback returns. A rejected contender must not touch the running callback.

- [ ] Adapt `DictionaryLookupController` through a private static thunk so the legacy path remains operational during migration:

  ```cpp
  static void runLookupJob(void* context) {
    static_cast<DictionaryLookupController*>(context)->runLookup();
  }
  ```

- [ ] Run the worker/native tests and all existing dictionary-related native tests:

  ```bash
  cmake --build build/tests -j2
  ctest --test-dir build/tests --output-on-failure -R 'JapaneseDictionary|DictLayout'
  ```

- [ ] Review checkpoint:

  ```bash
  git diff --check
  git status --short
  ```

### Task 8: Build the fixed-capacity horizontal page source

**Files:**

- Create: `src/activities/reader/PageTextSource.h`
- Create: `src/activities/reader/PageTextSource.cpp`
- Modify: `test/japanese_dictionary/JapaneseDictionaryTest.cpp`
- Modify: `test/japanese_dictionary/CMakeLists.txt`

**Interfaces:** Consumes an owned horizontal `Page`, renderer/font metrics, margins, and reserved-bottom geometry. Produces a read-only contiguous `PageTextSourceView` used by the scanner and renderer; later vertical/manga adapters can produce the same view.

- [ ] Write failing builder tests for CJK concatenation, ASCII word spacing, UTF-8 decoding, invalid sequences, line order, page-word ordinals, ruby shift, inserted hyphens, bionic word width, multi-word match bounds, orientation-independent screen coordinates, and allocation failure/truncation.

- [ ] Define the seam as plain value data, not virtual heap objects:

  ```cpp
  struct PageTextGlyph {
    uint32_t codepoint = 0;
    uint16_t paragraph = 0;
    uint16_t pageWord = 0;
    int16_t x = 0;
    int16_t y = 0;
    int16_t width = 0;
    int16_t height = 0;
  };

  struct PageTextSourceView {
    const PageTextGlyph* glyphs = nullptr;
    uint16_t glyphCount = 0;
    uint32_t contentHash = 0;
  };

  class HorizontalPageTextSource {
   public:
    DictionaryStatus build(const Page& page, GfxRenderer& renderer,
                           int fontId, int marginLeft, int marginTop);
    PageTextSourceView view() const;
    bool truncated() const;
    void clear();
  };
  ```

- [ ] Use two passes: count decoded codepoints, allocate one exact `PageTextGlyph[]` with `makeUniqueNoThrow`, then populate. If full allocation fails, release eligible font caches under the caller's `RenderLock` and retry once; otherwise allocate at most the first 256 glyphs, mark truncated, and keep reading functional.

- [ ] Match Matcha horizontal flattening: all page lines are paragraph `0`, insert a synthetic space only between adjacent ASCII word boundaries, and directly concatenate CJK runs. Synthetic separators receive a zero rectangle and are not selectable.

- [ ] Move CrossInk's current width/RTL/bionic/hyphen calculations from `DictionaryWordSelectActivity::extractWords` into `PageTextSource.cpp`, then have the temporary legacy activity call the shared helper until Task 14 deletes it. Give each decoded codepoint the source word's rectangle and union source-word rectangles when highlighting a multi-word candidate.

- [ ] Hash glyph count, codepoints, paragraph IDs, page-word ordinals, and rectangles with FNV-1a. Do not hash pointers.

- [ ] Run:

  ```bash
  cmake --build build/tests --target JapaneseDictionaryTest -j2
  ctest --test-dir build/tests --output-on-failure -R JapaneseDictionary
  ```

- [ ] Review checkpoint:

  ```bash
  git diff --check
  git status --short
  ```

### Task 9: Add progressive backend-specific page scanning

**Files:**

- Create: `src/activities/reader/PageWordScanner.h`
- Create: `src/activities/reader/PageWordScanner.cpp`
- Modify: `test/japanese_dictionary/JapaneseDictionaryTest.cpp`
- Modify: `test/japanese_dictionary/CMakeLists.txt`

**Interfaces:** Consumes `PageTextSourceView`, backend kind, and a non-owning probe callback. Produces a monotonically growing fixed candidate array and one immutable selected-candidate snapshot.

- [ ] Write failing tests with a fake probe for: StarDict token boundaries, Japanese longest match, noise filters, particles/counters, honorific grouping, grammar/name ordering, temporary end while incomplete, cycle only after complete, cancellation, retry after truncation, and no scan mutation during active definition work.

- [ ] Define the scanner contract:

  ```cpp
  struct PageWordCandidate {
    uint16_t firstGlyph = 0;
    uint8_t glyphCount = 0;
    uint8_t matchedBytes = 0;
    uint16_t firstPageWord = 0;
    uint16_t lastPageWord = 0;
  };

  struct DictionaryProbeFn {
    void* context = nullptr;
    DictionaryStatus (*call)(void*, const DictionaryQuery&, DictionaryProbeResult&) = nullptr;
  };

  class PageWordScanner {
   public:
    DictionaryStatus begin(PageTextSourceView source,
                           DictionaryBackendKind backend,
                           DictionaryProbeFn probe);
    DictionaryStatus stepOne();
    bool done() const;
    bool truncated() const;
    uint16_t candidateCount() const;
    const PageWordCandidate* candidate(uint16_t index) const;
    void restart();
    void clear();
  };
  ```

- [ ] Allocate candidates once with capacity equal to glyph count (a position can contribute at most one selectable candidate). Use `makeUniqueNoThrow`; on failure retry with `min(glyphCount, 256)` and mark truncated.

- [ ] For StarDict, emit existing whitespace/dash token units and probe exact tokens. For Japanese, port Matcha `WordSelectionScan` logic and call `DictionaryEngine::probe` with definition-free longest-at-offset queries. Candidate insertion is append-only; matched interiors advance `skipUntil`.

- [ ] Keep time policy outside the scanner: activity code repeatedly calls `stepOne()` until 50 ms elapse. This makes native tests deterministic and guarantees no hidden blocking loop.

- [ ] Run native tests and record the maximum candidate/glyph allocation sizes for a synthetic 800×480 dense page in the test log.

- [ ] Review checkpoint:

  ```bash
  git diff --check
  git status --short
  ```

### Task 10: Add the strong, atomic `wlscan.bin` cache

**Files:**

- Create: `src/activities/reader/PageWordScanCache.h`
- Create: `src/activities/reader/PageWordScanCache.cpp`
- Modify: `test/japanese_dictionary/JapaneseDictionaryTest.cpp`
- Modify: `test/japanese_dictionary/CMakeLists.txt`

**Interfaces:** Consumes a complete non-truncated scanner, backend kind, book position, glyph hash, and engine signature. Produces/restores candidate records and cursor only after full validation.

- [ ] Write failing tests for round-trip, backend mismatch, same-sized dictionary replacement, glyph/layout mismatch, spine/page mismatch, incomplete/truncated refusal, invalid cursor clamping, bad count, short payload, payload checksum mismatch, temp-file interruption, and backup recovery.

- [ ] Freeze the packed little-endian v2 header at 32 bytes:

  ```text
  u32 magic = 0x534C5743       # bytes "CWLS"
  u8  version = 2
  u8  backend                 # DictionaryBackendKind
  u16 flags                   # bit 0 = complete
  u16 spine
  u16 page
  u32 glyphHash
  u64 dictionarySignature
  u16 candidateCount
  u16 cursor
  u32 payloadFnv1a
  ```

  Each payload record is eight bytes: `u16 firstGlyph`, `u8 glyphCount`, `u8 matchedBytes`, `u16 firstPageWord`, `u16 lastPageWord`.

- [ ] Read/write fields explicitly in little-endian order; never serialize a C++ struct directly. Validate every candidate against the current source glyph count and monotonic order before publishing it to the scanner.

- [ ] Write `wlscan.bin.tmp`, close it, re-open and validate it, preserve an existing cache as `wlscan.bin.bak`, rename temp to final, then remove backup. On promotion failure restore the backup. Reuse the proven replacement pattern in `lib/Epub/Epub/Section.cpp:70` rather than inventing destructive replacement.

- [ ] Cache only `done() && !truncated()`. Treat every load failure as a cache miss after logging; never turn cache corruption into lookup failure.

- [ ] Run native tests.

- [ ] Review checkpoint:

  ```bash
  git diff --check
  git status --short
  ```

### Task 11: Port ruby glossary harvesting and invalidate old section generations

**Files:**

- Create: `lib/Epub/Epub/RubyGlossary.h`
- Create: `lib/Epub/Epub/RubyGlossary.cpp`
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h`
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`
- Modify: `lib/Epub/Epub/Section.cpp`
- Modify: `docs/file-formats.md`
- Modify: `test/japanese_dictionary/JapaneseDictionaryTest.cpp`
- Modify: `test/japanese_dictionary/CMakeLists.txt`

**Interfaces:** Parser produces bounded `(base,ruby)` pairs. Section merges them into Matcha-compatible `<book-cache>/ruby.bin`. Japanese result presentation consumes exact-base readings.

- [ ] Write failing tests for version-1 byte layout, mono-ruby compound merge, duplicate suppression, multiple readings joined with `・`, kanji requirement, 32-byte cap, 200-pair section cap, 1,024-record/16 KB file caps, corrupt/truncated whole-file rejection, and atomic rewrite after corruption.

- [ ] Port the public API unchanged from Matcha:

  ```cpp
  namespace RubyGlossary {
  using Pair = std::pair<std::string, std::string>;
  void collect(std::vector<Pair>& pairs, const std::string& base, const std::string& ruby);
  void merge(const std::string& bookCachePath, const std::vector<Pair>& pairs);
  bool lookup(const std::string& bookCachePath, const std::string& base,
              std::string& outReadings);
  }
  ```

- [ ] Preserve Matcha constants exactly: version 1, `ruby.bin`, max text 32 bytes, max 200 pairs per section, max 1,024 file records, max 16 KB. Guard vector capacity before growth and treat glossary loss as cosmetic.

- [ ] Add parser fields `rubyElemBase`, `rubyElemRuby`, `rubyElemRunCount`, and `rubyHarvest`; collect both individual ruby runs and compound mono-ruby at `</ruby>`. Merge only after a successful section parse/build, never after abort.

- [ ] Bump `SECTION_FILE_VERSION` from 61 to 62 and `SECTION_FILE_PARTIAL_VERSION` from `0xF8` to `0xF9`. Do not change serialized page fields. Document that this is a semantic invalidation so pre-glossary books reparse and populate `ruby.bin`.

- [ ] Add `JapaneseDictionaryBackend::bookReading(surface, out)` and set `DictionaryCapabilities::ruby` when a book cache path is present. The floating UI prepends translated `In this book: <reading>` only on a glossary hit.

- [ ] Run native tests and rebuild a ruby EPUB twice in the simulator: first build must create `ruby.bin`; second must load the section cache and preserve the glossary.

- [ ] Review checkpoint:

  ```bash
  git diff --check
  git status --short
  ```

### Task 12: Build the worker-driven definition model

**Files:**

- Create: `src/activities/reader/DictionaryDefinitionModel.h`
- Create: `src/activities/reader/DictionaryDefinitionModel.cpp`
- Modify: `src/util/DictLayout.h`
- Modify: `src/util/DictLayout.cpp`
- Modify: `test/dict_layout/DictLayoutTest.cpp`
- Modify: `test/japanese_dictionary/JapaneseDictionaryTest.cpp`
- Modify: `test/japanese_dictionary/CMakeLists.txt`

**Interfaces:** Consumes an engine definition handle and a fixed font-advance table. Produces an immutable current-page text/segment snapshot for render. All SD reads and wrapping happen on the worker; only font-cache preparation happens on the main loop under `RenderLock`.

- [ ] Write failing tests for plain Japanese, StarDict HTML styles/lists/IPA, malformed HTML plain fallback, page count, page turns, generation mismatch, read error, cancellation between chunks, codepoint-table cap, and immutable snapshot publication.

- [ ] Define a two-stage model so worker code never touches renderer/font caches:

  ```cpp
  enum class DefinitionBuildState : uint8_t {
    Idle, CollectingCodepoints, NeedsFontPrewarm, LayingOut, Ready,
    ReadError, OutOfMemory, Cancelled
  };

  struct DefinitionAdvanceTable {
    static constexpr uint16_t kCodepointCapacity = 256;
    std::array<uint32_t, kCodepointCapacity> codepoints{};
    std::array<std::array<int16_t, 4>, kCodepointCapacity> advances{};
    uint16_t count = 0;
  };

  class DictionaryDefinitionModel {
   public:
    void begin(DictionaryEngine& engine, DictionaryDefinitionHandle handle,
               int targetPage, int maxWidth, int linesPerPage);
    void collectCodepointsOnWorker();
    bool prewarmOnMain(GfxRenderer& renderer, int fontId, RenderLock& lock);
    void layoutOnWorker();
    DefinitionBuildState state() const;
    const DictionaryDefinitionPage& page() const;
    int totalPages() const;
    void cancel();
    void clear();
  };
  ```

- [ ] Generalize `DictLayout::Wrapper` measurement input so it can use the fixed advance table instead of calling `GfxRenderer`. Preserve the existing renderer adapter for current tests/legacy activity.

- [ ] First worker pass streams the definition only to collect up to 256 unique codepoints/styles. Main loop acquires `RenderLock`, prewarms the selected dictionary/reader/built-in fallback font, and fills advances. Second worker pass re-streams and wraps, counts every line, and copies only the target page into fallibly allocated exact-size line/segment/text arrays.

- [ ] Reuse a heap-owned 512-byte stream buffer. Check cancellation between every file chunk and layout line. Publish the snapshot with release/acquire ordering only after arrays and totals are complete; render never observes mutable builder state.

- [ ] If codepoints exceed the table, use the existing approximation/built-in fallback rules from `DictionaryDefinitionActivity` and log once per result. Do not allocate a larger table on C3.

- [ ] Run `DictLayoutTest` and `JapaneseDictionaryTest`.

- [ ] Review checkpoint:

  ```bash
  git diff --check
  git status --short
  ```

### Task 13: Implement and test the unified floating lookup flow

**Files:**

- Create: `src/activities/reader/DictionaryLookupFlow.h`
- Create: `src/activities/reader/DictionaryLookupFlow.cpp`
- Create: `src/activities/reader/EpubReaderWordLookupActivity.h`
- Create: `src/activities/reader/EpubReaderWordLookupActivity.cpp`
- Modify: `test/japanese_dictionary/JapaneseDictionaryTest.cpp`
- Modify: `test/japanese_dictionary/CMakeLists.txt`

**Interfaces:** Page mode consumes an owned `Page` plus reader callbacks/context. Direct mode consumes a history headword. Both modes own one engine session, worker jobs, definition model, and floating panel state; they return existing `ActivityResult`/clipping outcomes.

- [ ] Write pure `DictionaryLookupFlow` tests before activity code: initial burst, loading→ready, not-found/read-error/OOM/cancelled, discovered-next while incomplete, on-demand advance, cycle only when complete, cursor restore/clamp, lookup replacement, definition paging, and exit while worker-owned.

- [ ] Freeze the request objects to avoid another long constructor:

  ```cpp
  struct EpubLookupPageRequest {
    std::string bookLanguage;
    std::string bookCachePath;
    uint16_t spineIndex = 0;
    uint16_t pageIndex = 0;
    int marginLeft = 0;
    int marginTop = 0;
    int reservedBottomHeight = 0;
    int initialTouchX = -1;
    int initialTouchY = -1;
    bool autoLookupInitialWord = false;
    bool framebufferContainsPage = false;
    const char* dictionaryFontFamilyName = nullptr;
    uint8_t dictionaryFontPointSize = 0;
    void* readerContext = nullptr;
    void (*renderReaderBackground)(void*) = nullptr;
    std::unique_ptr<Page> (*reloadReaderPage)(void*) = nullptr;
  };

  class EpubReaderWordLookupActivity final : public Activity {
   public:
    EpubReaderWordLookupActivity(GfxRenderer&, MappedInputManager&,
                                 std::unique_ptr<Page>, EpubLookupPageRequest);
    EpubReaderWordLookupActivity(GfxRenderer&, MappedInputManager&,
                                 std::string directWord,
                                 EpubLookupPageRequest);
  };
  ```

- [ ] Port Matcha's floating-panel geometry, progressive header/count, first-result burst, navigation, scroll, loading/error states, and ten-fast-refresh cleanup cadence. Replace hardcoded dimensions with `renderer.getScreenWidth/Height()` and `UITheme` metrics; preserve all orientations and dark mode.

- [ ] In `onEnter`, open the engine, build/load the source/cache, record the open deadline, and return without a blocking scan. In the hot `loop()` initial-burst state, process input, run repeated `scanner.stepOne()` calls for at most 50 ms, and start the full lookup immediately when the first candidate appears. Continue loop ticks until scan completes or the 1,500 ms open deadline is reached. Log total open-to-ready time; the Task 1 fixture fails parity if its first definition is not ready within 1,500 ms, so the cap covers scanning, lookup, and definition-model publication rather than scanning alone.

- [ ] While lookup/model work is active, do not scan. On worker completion, perform font prewarm under `RenderLock`, resume worker layout, then publish/render the immutable definition page. When idle, continue 50 ms scan slices from `loop()`.

- [ ] Preserve CrossInk behavior: selected page-word rectangle highlight, clipping request, lookup history/status, StarDict suggestions/alternate forms, dictionary switching only when StarDict capabilities expose it, custom dictionary font/size, lookup inside definition text, touch/back/long-press semantics, and reader-background restoration callbacks.

- [ ] Add Japanese presentation: canonical headword, deinflected surface indicator, reading, source-aware vocabulary/names/grammar order, and optional `In this book` ruby line. Japanese sources remain one automatic backend and do not open the StarDict switcher.

- [ ] `onExit` order is mandatory: set cancellation, wait for worker ownership to clear, save only a complete scan/cursor, close engine/files, release Japanese caches, release source/model arrays, then call `Activity::onExit()`.

- [ ] Run native tests and `pio run -e simulator`.

- [ ] Manual simulator checkpoint: capture one Japanese and one StarDict screenshot in portrait and landscape; exercise touch if using a touch simulator profile and buttons in `simulator-X3`.

- [ ] Review checkpoint:

  ```bash
  git diff --check
  git status --short
  ```

### Task 14: Switch all EPUB lookup entries, migrate history, and remove the legacy flow

**Files:**

- Modify: `src/activities/reader/EpubReaderActivity.h`
- Modify: `src/activities/reader/EpubReaderActivity.cpp`
- Modify: `src/activities/reader/LookedUpWordsActivity.h`
- Modify: `src/activities/reader/LookedUpWordsActivity.cpp`
- Modify: `src/activities/reader/ReaderActivity.cpp`
- Modify: `src/SettingsList.h`
- Delete after parity checkpoint: `src/activities/reader/DictionaryWordSelectActivity.h`
- Delete after parity checkpoint: `src/activities/reader/DictionaryWordSelectActivity.cpp`
- Delete after parity checkpoint: `src/activities/reader/DictionaryDefinitionActivity.h`
- Delete after parity checkpoint: `src/activities/reader/DictionaryDefinitionActivity.cpp`
- Delete after parity checkpoint: `src/util/DictionaryLookupController.h`
- Delete after parity checkpoint: `src/util/DictionaryLookupController.cpp`

**Interfaces:** All current EPUB menu/touch/shortcut/history call sites produce `EpubLookupPageRequest` and launch the unified activity. No reader activity calls `Dictionary::*` directly after cleanup. `DictionarySuggestionsActivity` remains a child action used only by the StarDict-capable unified flow.

- [ ] Add an internal, non-persisted `CROSSINK_UNIFIED_DICTIONARY_DEV` compile gate around `EpubReaderActivity::openWordSelect` while comparing flows. Do not add it to release PlatformIO environments.

- [ ] Replace availability checks at `EpubReaderActivity.cpp:3259`, reader-menu construction around line 2330, touch entry at line 3263, menu entry at line 3497, and shortcut entry around line 4332 with `DictionaryEngine::open`/registry routing rather than `Dictionary::exists`.

- [ ] Build the request with `epub->getLanguage()`, `epub->getCachePath()`, `currentSpineIndex`, `section->currentPage`, current margins/status-bar reserve, touch coordinates, font settings, and existing background/reload callbacks. Put scan cache at `epub->getCachePath() + "/wlscan.bin"`.

- [ ] Pass `epub->getLanguage()` into `LookedUpWordsActivity`. Selecting history launches direct mode of the same floating activity; history storage format remains unchanged.

- [ ] Run the full parity matrix with the dev gate enabled: both backends, touch/buttons, clipping, lookup-chain, suggestions/alternate forms, dictionary switch, history, custom font, orientation, dark mode, cancellation, reader exit, sleep/resume, and another book.

- [ ] Only after every parity item passes, remove the dev gate and make the unified path unconditional. Delete the legacy activities/controller with `apply_patch`, remove their includes/references, and verify:

  ```bash
  rg -n 'DictionaryWordSelectActivity|DictionaryDefinitionActivity|DictionaryLookupController' src lib test
  ```

  Expected: no matches. `DictionarySuggestionsActivity` may remain.

- [ ] Confirm final reader-boundary compliance:

  ```bash
  rg -n 'Dictionary::' src/activities/reader
  ```

  Expected: no direct backend calls from reader activities; persistence-only settings/registry calls outside reader activities are allowed.

- [ ] Run simulator and native suites.

- [ ] Review checkpoint:

  ```bash
  git diff --check
  git status --short
  ```

### Task 15: Document, translate, and expose the feature safely

**Files:**

- Modify: `lib/I18n/translations/english.yaml`
- Regenerate: `lib/I18n/I18nKeys.h`
- Regenerate: `lib/I18n/I18nStrings.h`
- Regenerate: `lib/I18n/I18nStrings.cpp`
- Modify: `docs/file-formats.md`
- Create: `docs/japanese-dictionaries.md`
- Modify: `CHANGELOG.md`

**Interfaces:** Documentation consumes the frozen converter/cache contracts. UI consumes generated `STR_*` identifiers; no hardcoded user-facing lookup text remains.

- [ ] Add English source strings for automatic Japanese backend, fallback dictionary, source labels, `In this book`, scan/loading states, invalid dictionary, read error, OOM, and unsupported actions. Reuse existing keys where wording already matches.

- [ ] Run the generator rather than editing generated files:

  ```bash
  python3 scripts/gen_i18n.py
  ```

- [ ] Verify every user-facing string in new C++ uses `tr(STR_*)`:

  ```bash
  rg -n 'drawText|drawCenteredText|label\s*=' src/activities/reader/EpubReaderWordLookupActivity.cpp
  ```

- [ ] Document installation paths, preferred and legacy filenames, converter examples, optional names/grammar behavior, automatic `ja` routing, StarDict fallback, and C3 SD/font guidance in `docs/japanese-dictionaries.md`.

- [ ] Document the exact 40-byte `.idx`, `.spx` v1, `ruby.bin` v1, and `wlscan.bin` v2 fields and the section version 62 semantic invalidation in `docs/file-formats.md`.

- [ ] Add a human-facing `Added` entry to the current unreleased section of `CHANGELOG.md`: automatic Japanese vocabulary/names/grammar lookup with conjugation-aware longest matching and the unified floating panel.

- [ ] Review checkpoint for generated and handwritten changes:

  ```bash
  git diff --check
  git status --short
  ```

### Task 16: Run the parity, build, static-analysis, and hardware completion gates

**Files:**

- Modify only if a verified failure requires it: files already listed above.
- Record results in the implementation handoff; do not create a release commit or push.

**Interfaces:** Consumes the complete port. Produces objective pass/fail evidence required before manga work begins.

- [ ] Format only touched C++ sources/headers, using the repository style. Do not bulk-format unrelated files.

- [ ] Run converter and native tests from clean build directories:

  ```bash
  python3 -m unittest discover -s test/japanese_dict_converter -p 'test_*.py' -v
  cmake -S test -B build/tests
  cmake --build build/tests -j2
  ctest --test-dir build/tests --output-on-failure
  ```

- [ ] Run every required PlatformIO build:

  ```bash
  pio run -e simulator
  pio run -e default
  pio run -e sticky
  pio run -e x4-pro
  ```

- [ ] Run static analysis and the existing simulator smoke tripwire:

  ```bash
  pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high
  python3 scripts/run_simulator_smoke_test.py
  ```

- [ ] Compare firmware size and heap-shape logs against the Task 1 baseline. Explain mechanisms for every material change: static rule table increases flash; exact scanner/model arrays consume activity-lifetime heap; one reused worker avoids another 4 KB stack; accelerator caches are released on exit.

- [ ] On an ESP32-C3 X3 or X4, use a horizontal Japanese EPUB, vocab/names/grammar files generated by the converter, and an SD reader font. Clear the book's `.crosspoint/epub_<hash>/` once to force version-62 parse. Verify:

  - cold first valid definition appears within the 1.5-second burst cap;
  - each logged scan slice is at most 50 ms;
  - candidates/headwords/readings/deinflection/source order/definitions match Matcha fixtures;
  - 20 open/navigate/close cycles show no monotonic drop in `ESP.getMaxAllocHeap()`;
  - page turn, repagination, another lookup, sleep/resume, and a second book still work;
  - missing/corrupt/replaced files fall back or report errors without restart/abort.

- [ ] On Sticky or X4 Pro, repeat Japanese and StarDict flows using touch: initial word tap, previous/next, scroll, clipping, dismissal, every supported orientation, and dark mode. Check free/largest PSRAM separately from internal/DMA-capable heap.

- [ ] Run final repository checks:

  ```bash
  git diff --check
  git status --short
  git diff --stat
  ```

- [ ] Review checkpoint: inspect the final status and diff output, verify only planned paths changed, and leave all changes uncommitted and unpushed.

- [ ] Declare dictionary parity complete only if all automated commands and both hardware gates pass, no legacy lookup references remain, cache/setup docs and changelog are present, and unrelated files remain untouched. Only then begin the separate manga implementation plan.
