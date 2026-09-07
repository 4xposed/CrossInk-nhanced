### Spec Compliance

- ✅ Spec compliant for the Task 9c implementation scope. The sampled `signature()` API remains separate; full canonical identity is incremental and activity-owned (`src/util/DictionaryEngine.cpp:375`, `lib/Dict/DictIndex.cpp:736`, `src/util/StarDictBackend.cpp:397`, `src/activities/reader/EpubReaderWordLookupActivity.h:194`).
- ✅ Canonical index bytes, Japanese source routes/availability/data extents, and StarDict index/synonym/metadata plus route/data extents participate. Definition bodies and rebuildable accelerators do not participate (`lib/Dict/DictIndex.cpp:741`, `src/util/StarDictBackend.cpp:383`, `src/util/StarDictBackend.cpp:407`, `src/util/StarDictBackend.cpp:452`).
- ✅ New entry/picker activation resets verification; matching internal reopen retains progress or a verified digest. Failed resume revokes trust, and changed routes discard old candidates (`src/activities/reader/EpubReaderWordLookupActivity.cpp:349`, `src/activities/reader/EpubReaderWordLookupActivity.cpp:1162`, `src/activities/reader/EpubReaderWordLookupActivity.cpp:2082`, `src/util/DictionaryEngine.cpp:396`).
- ✅ The initial opportunity is bounded by 16 steps of at most 256 bytes and a cooperative 5 ms limit. Subsequent eligible chunks use the existing yield path; work is excluded while either flow or actual worker owns the backend (`src/activities/reader/DictionaryScanIdentityPolicy.h:8`, `src/activities/reader/EpubReaderWordLookupActivity.cpp:930`, `src/activities/reader/EpubReaderWordLookupActivity.cpp:1769`, `src/activities/reader/EpubReaderWordLookupActivity.cpp:2109`, `src/activities/reader/EpubReaderWordLookupActivity.cpp:2129`).
- ✅ Ready-only cache gates preserve progressive candidates and require complete, untruncated scans for saving (`src/activities/reader/EpubReaderWordLookupActivity.cpp:416`, `src/activities/reader/EpubReaderWordLookupActivity.cpp:841`, `src/activities/reader/EpubReaderWordLookupActivity.cpp:2141`). The actual cold/warm activity path restores cursor 1 through reader input (`src/simulator/SimulatorSmokeTest.cpp:745`, `src/simulator/SimulatorSmokeTest.cpp:1338`; `/private/tmp/crossink-manga-ocr9c-smoke.log:191`, `:222`, `:234`).
- ⚠️ Physical X4 and S3 behavior, real SD verification latency, heap fragmentation/worker stack watermarks, and restoration of the user's dictionary backup cannot be established by this diff. The root must retain those completion checks from `.superpowers/sdd/2026-09-06-manga-ocr-plan/task-9c-brief.md:45`. Test repeated panel/overview lookup, nested definitions/history/back, clipping and cancellation with both dictionaries and user SD fonts; measure first visible definition separately from verification, and confirm image/font restoration.
- ⚠️ The large-index cache-load bypass is an explicit accepted tradeoff, not a measured hardware speed claim (`docs/file-formats.md:404`, `src/activities/reader/EpubReaderWordLookupActivity.cpp:841`). Active-file immutability and transfer-screen quiescence remain the binding session assumption; this task does not prove every transfer integration.

### Strengths

- Trust cannot escape through a moved-from, failed, cancelled or partially read state. Digest access is status-gated and final publication gets its own cancellation boundary (`lib/Dict/DictionaryScanIdentity.h:18`, `:39`, `:75`; `lib/Dict/DictIndex.cpp:778`; `src/util/DictionaryEngine.cpp:382`).
- Identity-only allocation/path failures preserve ordinary StarDict lookup, and unreadable optional Japanese sources disable caching without discarding the usable vocabulary backend (`src/util/StarDictBackend.cpp:121`, `lib/Dict/DictIndex.cpp:583`, `:593`; `test/japanese_dictionary/JapaneseDictionaryTest.cpp:6917`, `:6937`).
- New buffers are avoided in the hot chunk path: Japanese reuses its existing 256-byte scratch and open handles; StarDict reuses the session stream buffer and explicitly closes each canonical reader (`lib/Dict/DictIndex.cpp:785`, `src/util/StarDictBackend.cpp:383`, `:473`).
- Canonical read accounting and injected cancellation/error tests exercise real files and backend operations, including sparse 100 MiB bodies and middle-only edits (`test/japanese_dictionary/JapaneseDictionaryTest.cpp:6658`, `:6741`, `:6772`, `:7051`).

### Issues

#### Critical (Must Fix)

- None found in the Task 9c snapshot.

#### Important (Should Fix)

- None found in the Task 9c snapshot.

#### Minor (Nice to Have)

- `test/japanese_dictionary/JapaneseDictionaryTest.cpp:6870`: the large progressive-scan test finishes identity through the engine while its scanner and flow are independent local objects. Its cursor/candidate assertions therefore cannot catch an accidental late activity cache load. The production bypass guard is explicit and correct (`src/activities/reader/EpubReaderWordLookupActivity.cpp:841`, `:2141`), so this is nonblocking coverage debt. A future activity harness extension could use a dictionary exceeding the initial slice and an existing cache with a different cursor, then assert the live cursor/candidates survive Ready publication; no additional test batch is required for this approval.

### Assessment

- **Task quality: Approved.** No blocking spec or correctness defect was found. Ownership, bounded work, fallback behavior and cache trust gates are coherent; physical validation remains with the root.
- **Scope checked:** `/private/tmp/crossink-manga-ocr9c-review.patch`, all 2,233 lines, against the supplied pre-9c snapshot; this is not a whole-branch review. No source edits, Git commands/mutations, PIO, serial access or test executions were performed by this reviewer. Only this requested review report was written.
- **Named dependency check — reopen/worker overlap:** the diff cuts off activity command and picker bodies, so CodeGraph supplied those bodies. Cancel/join precedes reopen at `src/activities/reader/EpubReaderWordLookupActivity.cpp:725`; the picker closes the backend before child use and restarts verification/rescan at `:1115`, `:1162`, `:1198`.
- **Named dependency check — render use-after-free during route reset:** CodeGraph supplied render/background/header/snapshot bodies beyond the diff. Render uses copied candidate highlight state (`src/activities/reader/EpubReaderWordLookupActivity.cpp:883`, `:2028`); the external source borrowed at `:1839` remains unchanged, and EPUB source rebuilding holds RenderLock at `:286`. No candidate pointer is retained by these render paths.
- **Named dependency check — OOM route comparison:** `DictionaryOwnedText::c_str()` returns a valid empty string when storage is absent (`src/util/DictionaryEngineTypes.h:101`), so a failed retained route cannot pass null to StarDict's resume comparison.
- **Evidence inspected, not rerun:** `/private/tmp/ocr9c-full-tests.log` records 302 tests across 14 suites passing in 259 ms; `/private/tmp/ocr9c-build.log` ends with successful linking and contains no warnings. Root's final `/private/tmp/crossink-manga-ocr9c-native-tests.log` records all 675 tests passing in 31.10 s. The final button OCR log contains actual cold `loaded=0 cursor=0`, warm `loaded=1 cursor=1`, and success at 28.845 s; `/private/tmp/crossink-manga-ocr9c-epub-smoke.log` ends in success at 3.255 s.
- **Known diagnostic limitation:** the implementer reports pre-existing unused constants in `Dictionary.cpp`, and the root reports expected missing-cover/cache fixture diagnostics. Those are acknowledged validation noise, not new Task 9c findings; physical/simulator limitations must not be hidden by a blanket clean-output claim.
- **Final touch evidence:** `/private/tmp/crossink-manga-ocr9c-touch-smoke.log` records the cold/warm verified cache assertions and simulator success at 29.136 s. Large-index Pending timing and cancellation on physical hardware remain a completion gate.
