# Manga port final software verification — 2026-09-07

The software port is complete for the reviewed scope. Fresh validation is complete,
and the final scoped review gives specification PASS and quality PASS, with all
nine findings addressed and the Python validation correction accepted. Physical
acceptance and flashing remain separate pending work.

Work remains uncommitted on `matcha_features`, based on dictionary-work HEAD
`ea03940023c1705fed80ea64cc0de3208f38321a`. No staging, commit, amend or push was
performed. SDK pin remains `1e8ee543edca397f2b8747811f5a88f1bc35d233`.

## Scope and review evidence

The [whole-port review](2026-09-07-manga-final-review.md) covered all 50 original
handoff checkboxes, every recorded ruling/deferral, 363 manifest entries, 13 binary
fixtures and the complete implementation/test/public-documentation diff. Its six
Important and three Minor findings were addressed in one combined correction:
[fix report](2026-09-07-manga-final-fix-report.md). The
[scoped review](2026-09-07-manga-final-fix-review.md) covers the 3,564-line,
44-path correction and the subsequent 174-line Python-only validation supplement.

Corrections cover active-upload cancellation, sparse/mixed page identity,
journalled Recent deletion, retained partial completion saves, sleep-cover
cancellation, ordinary PDF test cleanup, documentation and two small cleanups.
The port includes the completed reader/menu, OCR/dictionary integration, covers,
progress/statistics/languages and managed file-transfer/library integrations;
individual mechanisms and deliberate limits remain in the linked task reports.

Original 307-file preimages remain intact. The final accepted source/test/public
manifest contains 316 files at
`/private/tmp/crossink-manga-final-accepted-hashes.json`. After firmware freeze,
only the two approved Python validation files changed; no C/C++ or firmware input
changed during or after the successful firmware builds.

## Fresh verification

| Gate | Result |
| --- | --- |
| Full native build and sequential CTest | PASS, 811/811, 66.08 seconds |
| Updated stress-validator tests | PASS, 9 Python test methods; registered CTest rerun 1/1 |
| Ordinary offline converter discovery with installed PDF support | PASS, 10/10, 0.228 seconds |
| Normal manga, OCR, grayscale/prefetch stress, full gray menu, failure/retry and EPUB | PASS, all 12 logical jobs across button/touch profiles |
| Real localhost WebSocket upload interrupted by managed Home/pop/sleep | PASS, 6/6 across both profiles |
| C3 plus button/touch simulator builds | PASS, all three environments, 117.922 seconds |
| Sticky and X4 Pro firmware builds | PASS, both environments, 343.519 seconds |
| Changed-path static analysis | Exit 1: 0 high, 6 medium, 57 low reports; 60 unique diagnostics across 162 filtered paths |
| Whitespace and source preservation | `git diff --check` clean; accepted manifest records final hashes |

The initial two stress runs failed because the Python harness required a debug
message removed in Task10e. Those failed logs are retained. The correction checks
ordered held-source → cancelled completion → activity transition instead,
preserves all other markers and downstream error/grayscale/progress/library
validation, and has negative tests for missing or misordered events. Both fresh
reruns then passed (35.205 and 35.304 seconds); the initial failures are not counted
as passes. Other already-passing workflows were not repeated without cause.

All 60 final static diagnostics were inspected against the original 51. The
uninitialized dither row warning is gone. Newly surfaced cancellation warnings
mistake borrowed mutable callbacks/deadlines for pure reads; the checkpoints are
required across I/O. Remaining diagnostics are documented style, capability or
typed-owner inference cases, with no new unsafe defect found. The static command
is **not clean**, and no warnings were globally suppressed. Ordinary PDF tests
also retain the upstream SWIG `__module__` deprecation warning.

Commands and exact results are retained under
`/private/tmp/crossink-manga-final-verification/` and the sibling
`crossink-manga-final-verification-*.log` files. `smoke-results.json` retains the
initial failures; `stress-rerun-results.json` supplies their accepted replacements.
`upload-results.json` records the six real network runs. WebSocket callbacks are
pumped on the simulator owner loop; unrelated simulator HTTP callbacks still use
a listener thread. The WS tests use disposable fixtures and keep the socket open
through cancellation, completing in 0.377–0.655 seconds under a four-second gate.
These numbers are simulator results, not hardware latency claims.

## Final artifacts

All app partitions are 6,553,600 bytes. Sizes below are the final complete builds.

| Target | Firmware bytes | Partition bytes remaining | SHA256 |
| --- | ---: | ---: | --- |
| default | 6,504,800 | 48,800 | `62c638e467ff8ed5776c59ea310c0e498fd1dd080cd2eaf2004a83617e6fec91` |
| sticky | 6,282,528 | 271,072 | `9683cc91e8a042c74763d7d7b1969b95470005fcf1976e3595977cdfe986cd5c` |
| x4-pro | 6,379,152 | 174,448 | `80240ac8573c0e3069ef311e739d7e632af24419d5f1c248688eda042a49aa99` |

Full bin/ELF hashes and build paths are in
`/private/tmp/crossink-manga-final-verification/artifacts.json`. The C3 artifact is
also saved as `/private/tmp/crossink-manga-final-firmware.bin` and its matching ELF
as `/private/tmp/crossink-manga-final-firmware.elf` for later upload/debugging.
**These artifacts have not been flashed.** The last user-confirmed reader image
is Task7; later build success does not update the device.

## Remaining physical work

At the final serial enumeration only Bluetooth/Px8S2/SonosAce/debug-console ports
were present; the reader endpoint was absent. Existing flash authorization still
applies when the C3 reader returns. Match the saved SHA256 to the upload, preserve
NVS/SD, and capture boot/status before claiming a successful flash. No S3 hardware
acceptance has been recorded.

Minimum device checks after flashing:

1. Open a known Matcha manga; check full-page/panel navigation, Back to overview,
   reopen at the same panel, rotation, chapter/page jumps and bookmarks.
2. Exercise the reader menu, automatic turns and cancellation, screenshot, and
   original-OCR QR. Check grayscale cleanup and touch gestures on an S3 device.
3. Use a disposable sparse/mixed-extension book to verify physical page identity,
   missing-overview crop fallback and OCR alignment.
4. Retry deliberately failed completion saves and delete/move disposable books
   from both Recent layouts and Files; confirm counts change once and durable
   state follows the shared mutation policy.
5. Interrupt a throwaway upload using Calibre Back, Home and sleep. Check the
   transition completes, partial-file policy applies and normal transfers resume.
6. Record sleep-cover cancellation, SD timing, heap/largest allocation and worker
   stack watermarks on C3 and S3. Large-dictionary first-definition latency and
   two-device Nearby updated/legacy interoperability still need hardware checks.

Use disposable fixtures for failure injection and delete only their derived
caches when a cold comparison is required. Do not clear user dictionary, bookmark,
progress, history, font or statistics data to make a check pass. Simulator image
stubs do not prove physical JPEG/PNG quality; native real-codec tests cover pixels
and faults, while display/SD timing remains physical acceptance.

## Dictionary preservation and unresolved restoration

The original Japanese dictionary remains at
`/dictionaries/jp.user-backup-20260905` (272,117,145 bytes across nine files).
Active `/dictionaries/jp` is still the small test fixture. User NotoSansJP and
BookerlyJP fonts remain untouched.

Automatic approval review rejected exporting that private backup to Mac temporary
storage because preservation alone did not authorize the 272 MB copy. No export
or restoration occurred. The existing explicit-approval question remains pending;
“continue”, “connected” and status requests are not treated as approval. Do not
rerun or bypass the rejected operation, or overwrite/delete the original backup.
Physical acceptance and that restoration are not claimed complete by this report.

The durable task decisions remain in
`.superpowers/sdd/2026-09-06-manga-library-completion-plan/progress.md`; the workspace
and review evidence are retained because no Git commits were permitted.
