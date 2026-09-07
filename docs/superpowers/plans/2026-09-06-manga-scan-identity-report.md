# Task 9c — verified persistent dictionary scan identity

2026-09-07. Implementation, focused native verification and final actual-activity
cold/warm button and touch simulator assertions pass. Independent review found no
blockers. Task 9c is DONE; root owns subsequent hardware-target builds and physical checks. No PIO,
serial, external OCR/downloads, SDK/font edits, staging, commits, amendments or pushes
were performed by this implementer. The dirty tree and earlier dictionary/manga work
were preserved. Existing files not already present in the root's 20-file snapshot
were copied before editing to `/private/tmp/crossink-manga-ocr9c-before/`.

## Exact changed files

Existing files modified by 9c:

- `lib/Dict/DictIndex.h`, `lib/Dict/DictIndex.cpp`
- `src/util/DictionaryEngine.h`, `src/util/DictionaryEngine.cpp`
- `src/util/JapaneseDictionaryBackend.h`
- `src/util/StarDictBackend.h`, `src/util/StarDictBackend.cpp`
- `src/activities/reader/EpubReaderWordLookupActivity.h`, `src/activities/reader/EpubReaderWordLookupActivity.cpp`
- `test/japanese_dictionary/JapaneseDictionaryTest.cpp`, `test/japanese_dictionary/stubs/HalStorage.h`
- `src/simulator/SimulatorSmokeTest.cpp`, `scripts/run_manga_simulator_smoke_test.py`
- `CHANGELOG.md`, `docs/file-formats.md`

New files:

- `lib/Dict/DictionaryScanIdentity.h`
- `src/activities/reader/DictionaryScanIdentityPolicy.h`
- This report.

No build-list change is necessary: the shared state and scheduling policy are headers,
and backend implementation extends existing compilation units. Only the appended test
slice was formatted, preserving the earlier test work.

## API, identity and ownership

`DictionaryEngine.cpp:375–402` exposes separate `beginScanIdentity`,
`stepScanIdentity(state, byteBudget)` and `resumeScanIdentity` operations. Cancellation
belongs to the activity-owned state's `cancel()`. Existing inexpensive `signature()`
implementations and golden signature tests remain unchanged. The new uint64 fingerprint
uses a policy/version domain, explicitly little-endian descriptors and length-delimited
strings; it is a content fingerprint rather than cryptographic integrity.

`DictIndex.cpp:736–798` hashes every byte of each active vocabulary, grammar and names
index, exact compiled resolved paths, source availability and data extents. It reuses
already-open index handles and the existing 256-byte `signatureScratch`, without opening
another reader for those paths. Optional-source read failures remembered at `:541,583,593`
disable verification while leaving a usable vocabulary backend intact. Missing optional
data makes that source inactive, so adding/removing its availability changes identity.

`StarDictBackend.cpp:397–484` captures exact base path, full `.idx`, optional `.syn`, full
`.ifo`, parsed word/index/type descriptors and `.dict` presence/size. Each identity read
opens/seeks/reads/closes one file, including every failure and cancellation path. The
existing 512-byte stream buffer first constructs the path and then receives at most
256 bytes. `.dat` and `.dict` payloads are never read by the new operation. This does
not claim ordinary open/signature avoids body reads: its original sampling is preserved.

Rebuildable `.spx`, `.qidx`, `.idx.oft`, `.syn.oft`, `.idx.oft.cspt` and `.syn.oft.cspt`
are excluded from both hash and resume descriptors. Their lazy creation/replacement
therefore cannot invalidate an activation. Existing accelerator coherence remains a
separate concern; fingerprinting them would not fix stale accelerator behavior.

`DictionaryScanIdentityState` owns fixed cursor/hash/descriptors, optional owned StarDict
path, and no file handle, worker, renderer or global memo. It measures **112 bytes on
this 64-bit native host**; the target activity size remains measurable in the existing
entry log. Moves explicitly revoke the moved-from digest. A failed route resume also
revokes trust. Publication is a separate bounded step after the final payload read,
allowing cancellation at that last boundary.

The activity retains Ready and Pending identities across cancel/join/reopen and nested
lookup for matching routes (`EpubReaderWordLookupActivity.cpp:2082`). New top-level entry
resets identity; a successful picker choice starts fresh verification, including choosing
the same route. Source mismatch discards candidate cache and restarts scanning. Failed
verification remains cache-disabled through ordinary internal reopen; deliberate picker
selection permits a fresh route. Failure never clears a usable engine's definition or
turns an identity-only OOM into dictionary-unavailable UI.

## Scheduling and cache gates

`DictionaryScanIdentityPolicy.h` supplies the same eligibility predicate to work and
`skipLoopDelay` (`EpubReaderWordLookupActivity.cpp:932,2109`). Pending work must have an
open backend, no exit, no flow worker, and no actual static worker. After its initial
opportunity it additionally waits for Ready/NotFound definition or a complete scan with
no selection, and for initial touch selection to resolve.

The activity polls input before `runInitialIdentitySlice()` (`:1770,2129`). The opening
opportunity is at most **16 steps × 256 bytes**, stopping at a cooperative **5 ms** elapsed
limit and checking Back/exiting/worker eligibility between steps. A slow physical SD read
can exceed this limit; it is not a hard latency guarantee. Progressive scanning latches
load bypass (`:841`) and gets priority over subsequent hash chunks. If the scanner uses
its 50 ms allowance, that turn omits identity work. Otherwise at most one eligible chunk
runs after scanner/completion commands. Existing `skipLoopDelay` selects RTOS `yield()`
between subsequent turns; input is polled between them. Pending but definition/worker/
touch-ineligible state adds no new busy-spin reason.

Only Ready identity can load or save. Cache loading occurs only in the initial opportunity
before candidates/selection exist (`:2141`); late verification cannot replace live candidates
or cursor. Saving still requires an untruncated source and complete, untruncated scanner
(`:416`). The same verified gate now enables external manga and existing EPUB sources.
Shutdown never finishes hashing: it saves only already-verified complete state, cancels,
closes and releases. The existing normal/forced lifecycle contracts are retained.

No identity work takes RenderLock or runs on the dictionary worker. Existing HAL physical
read serialization remains in force; no render gate, extra framebuffer, SDK or font
behavior was introduced. The scanner also checks the actual static worker's busy state.

## Allocation and extent limits

Japanese identity adds no heap buffer/allocation; fixed metadata lives with the activity
and existing backend scratch is reused. StarDict retains one bounded route in its backend
and one activation-owned route across backend destruction, each at most **507 bytes
including NUL**. These are fallible session/activation allocations because stack storage
cannot survive return and static storage would retain per-session paths indefinitely.
There is no per-chunk buffer/path allocation in app code. HAL still allocates its normal
short-lived handle implementation when StarDict opens a canonical file.

Routes longer than 506 bytes or route-allocation failure disable only identity caching;
ordinary lookup keeps its prior behavior (tested with a valid 507-byte route). Canonical
index/metadata/synonym extents over UINT32_MAX are rejected for verification rather than
wrapping the 32-bit target seek cursor. The existing HAL `fileSize64()` API captures true
definition-file size without body reads. No support for searching >4 GiB indexes is claimed.

## Tests and evidence

Final focused command sequence:

```sh
cmake --build /private/tmp/crossink-manga-ocr9b-test --target JapaneseDictionaryTest -j1
/private/tmp/crossink-manga-ocr9b-test/japanese_dictionary/JapaneseDictionaryTest
python3 -m py_compile scripts/run_manga_simulator_smoke_test.py
git diff --check
```

The existing disconnected build reuses
`/private/tmp/crossink-manga-pixel-build/_deps/googletest-src`; nothing was downloaded.
**302 tests across 14 suites passed in 259 ms**. Logs are `/private/tmp/ocr9c-build.log`
and `/private/tmp/ocr9c-full-tests.log`. Existing Dictionary.cpp compilation warnings
about two unused constants are unchanged. `git diff --check` passed and final status was
captured at `/private/tmp/ocr9c-status.log`.

TDD evidence: the first new test failed to compile because the new API was absent
(`/private/tmp/ocr9c-red.log`); policy test likewise failed before the policy existed.
Subsequent executable regression tests exposed moved-from trust, identity-route OOM
breaking lookup, ignored optional Japanese read failure, and a failed resume leaving
an old digest trusted. The fixes were followed by successful focused/full runs. Logs
include `/private/tmp/ocr9c-edge-red.log` and `/private/tmp/ocr9c-resume-red.log`.

The new tests at `JapaneseDictionaryTest.cpp:6607` onward cover:

- 48/49, 64, 96/97 and 4096 records; same-size middle headword, priority and POS edits.
- Full index byte counts, max 256 bytes/read, no second Japanese reader, no accelerator
  reads, and zero new-operation payload bytes from sparse **100 MiB** `.dat`/`.dict` bodies.
- Grammar availability, names read failure, preferred-path changes, data extent changes,
  StarDict route changes, full >2 KiB index/synonym/metadata edits and 32/64-bit metadata.
- Same-size definition-body replacement leaves candidate identity stable while the selected
  streamed definition changes; Found and NotFound probes preserve candidate behavior.
- Pending progress across repeated cancel/reopen, Ready reuse without rehash, new activation
  full rehash, accelerator lazy creation/replacement stability, move invalidation.
- Every Japanese chunk cancellation boundary including after the last read, cancellation
  during physical read on both backends, seek/short-read/open/OOM failures, explicit StarDict
  step closure and no partial digest trust.
- Real small three-source dictionary/cache second activation: **four steps**, restored
  cursor 1/candidate count 2. This native test composes the real engine, scanner and cache
  with the production scheduling policy; it does not instantiate the UI activity.
- A 4,098-record dictionary exhausts the initial slice, scans progressively and preserves
  its live cursor/candidate array when verification later completes; verified save succeeds.
- Shared eligibility: worker/actual-worker/touch/first-definition/exit/closed/terminal states,
  complete-without-selection case, and a bounded state-size assertion.

The final simulator harness now explicitly waits for the **actual activity** to have a
verified complete cold scan, asserts no cache was loaded, closes at candidate cursor 1,
checks the same physical page/panel/image, reopens through Confirm, and asserts a real
verified cache load with cursor 1. Markers are `Verified manga scan cache loaded=0 cursor=0`
and `Verified manga scan cache loaded=1 cursor=1`; the Python runner requires both plus
the activity's load-success marker. Read-only SIMULATOR accessors inspect real state;
no cache, candidate or flow state is injected. The root's final button run passed this
actual-activity assertion, including cold `loaded=0 cursor=0` and warm `loaded=1 cursor=1`.

The root reported intermediate (pre-final cache assertion) software passes: button manga
OCR 28.933 s, EPUB 3.396 s and Sticky simulator build 22.303 s. These are regression
checkpoints, not evidence that the final cold/warm assertion has passed yet.

## Remaining hardware verification and tradeoffs

Strict content validation can finish after a large dictionary's progressive scan; its
warm cache may therefore be bypassed on every activation. The priority remains time to
first definition; completed verification can still permit saving. Native fixture timing
is not SD throughput evidence and does not predict the user's roughly 76 MiB indexes.
Log first-definition time separately from `Dictionary scan identity verified after ...`
and cache eligible/bypassed/loaded status. A single slow read is not preemptible.

Files are assumed immutable while lookup is active. Transfer/storage mutation must quiesce
lookup; per-activation verification cannot detect arbitrary in-session external edits.

Root should verify X4 with Japanese and StarDict plus user SD fonts: repeated panel and
overview opens, translation, nested definitions, suggestions/history/back, long Confirm
clipping, picker switches, cancellation and exit during Pending hashing. Record first
visible definition vs full-index verification, free/largest internal heap and dictionary
worker stack watermark; confirm physical highlight/image/font restoration. Restore the
user's dictionary backup safely before everyday-use handoff. Repeat physical S3 touch/image
checks when available. This implementer did not access hardware; target builds and native/
simulator checks alone cannot establish physical e-ink, SD latency or memory pressure.

## Self-review: route-change ownership invariant

The new internal-reopen route-mismatch branch clears main-task scanner/cache candidates
outside RenderLock. This is safe with the current ownership contract: `render(RenderLock&&)`
at `EpubReaderWordLookupActivity.cpp:2028` reads the copied `RenderSnapshot` and its value
highlight coordinates, not scanner/cache/candidate pointers. `selectedCandidate()` is used
by main input/command/snapshot code; `publishRenderSnapshot():883` computes the copied
highlight under its existing lock. `runWorker():544` consumes owned query/results/model
and is joined before the reopen branch. The renderer's only `sourceView()` borrow is the
external-background callback at `:1839`; that external owner is unchanged by route restart.
EPUB source rebuilding remains inside the existing RenderLock at `:286–289`, and EPUB
background drawing uses the retained `page_` directly. No new render-deferral mechanism or
hash-under-lock operation is needed. This invariant was independently checked by root.


## Final root-run software results

- Button manga OCR including the new actual-activity cold/warm verified cache and cursor
  assertion: **PASS, 28.845 s**, `/private/tmp/crossink-manga-ocr9c-smoke.log`.
- Existing EPUB smoke: **PASS, 3.255 s**,
  `/private/tmp/crossink-manga-ocr9c-epub-smoke.log`.
- Full native suite, serialized under root ownership: **675 tests PASS, 31.10 s**,
  `/private/tmp/crossink-manga-ocr9c-native-tests.log`.

- Touch manga OCR, including the actual-activity cold/warm cache assertion: **PASS,
  29.136 s**, `/private/tmp/crossink-manga-ocr9c-touch-smoke.log`.

Root reported these results directly and reports that independent review found no blockers.
Task 9c is DONE. Production source is held unchanged for root's hardware-target builds;
physical hardware measurements and final everyday-use handoff remain root-owned.
