# Manga foundation review — 2026-09-05

Scope: design and implementation-plan Tasks 1–3 only. Read-only review of the new
allocation-free decoder, native tests/original-writer fixtures, converter diff and
offline tests. No reader/library/prefetch/dictionary implementation parity is implied.
Reviewers did not rerun implementers' test suites or use network/OCR services.

## Finding resolved during review

**P2 — zero-length page records must ignore their data offset.**
`lib/MangaPanel/MangaFormat.cpp:51` initially rejected `dataOffset > dataSize`
even when `dataLength == 0`. The pinned reader returns success immediately for
zero-length pages before opening or seeking `panels.dat`
(`../matcha-reader/lib/MangaPanel/MangaPanel.cpp:362`). This means an otherwise
valid empty-page record with a nonzero/out-of-file offset was rejected by the new
decoder. Parent independently identified the same case and notified the implementer.
Retain extent checking for nonempty records, preserve empty records and test the
ignored offset explicitly. Resolved: the implementer now applies the extent check only when length is nonzero
(`MangaFormat.cpp:58`), and added `ZeroLengthPageIgnoresUnusedOffsetLikeUpstream`
using offset UINT32_MAX with dataSize zero. Source and regression reviewed; test-run
evidence belongs to the implementer/parent validation report. No unresolved blocking
code finding remains in this review scope.

## Reviewed strengths and evidence

- Byte-wise LE loads avoid unaligned access; extent checks use subtraction rather
  than overflowing offset-plus-length. Parser allocates no memory and returns
  borrowed spans/views with documented lifetimes. Page and TOC parsing validates all
  records before publishing views; cursor failures clear output without advancing.
- Limits match pinned reader policy: 1–10000 pages, at most 32768 page bytes,
  1000 TOC entries, u8 panel/text counts and u16 lengths/coordinates. Reserved and
  trailing extension bytes are preserved/ignored as documented. Decoder does not
  silently clamp rectangles or assume NUL-terminated text.
- Fixtures were produced independently of the port: `test/manga_format/fixtures/generate.py`
  asserts pinned HEAD and exact `git show` equality, then imports the **original**
  converter. The original writer SHA256 and all six fixture SHA256 values were
  independently recomputed during review and match `fixtures/README.txt`.
- Native tests inspect original-writer Japanese OCR, embedded NUL, translation,
  page dimensions, metadata with/without language and TOC. Expanded tests also cover
  count/size boundaries, 255-record iterations, unaligned buffers, reserved/trailing
  bytes, optional-file corruption and non-advancing error cursors.
- Converter diff preserves the pinned script except attribution/setup/version/recovery
  prose and the designed OCR correction. The correction uses margin-expanded crop
  origin and converts endpoint rectangles to width/height before `encode_page`.
  This matches the binary writer's existing xywh contract. Existing converted bytes
  remain readable; only newly generated OCR coordinates change.
- The OCR regression exercises real panel detection, crop generation, resize,
  request/response mapping and binary encoding with mocked external subprocess output.
  It checks nonzero crop origin, normalized rectangle and missing-box fallback.
  Other offline tests cover literal bytes, metadata/TOC, natural/explicit order,
  duplicate archive basenames/traversal, EPUB spine, fit, mono and baseline JPEG.
  Network/model boundaries are blocked in tests; no user key is loaded.

## Follow-ups and limits, not blockers for this slice

- Optional-file strictness differs deliberately from upstream: TOC version is now
  checked, a truncated language trailer invalidates metadata, and language is
  exposed at its full encoded length rather than upstream's 16-byte display cap.
  Document this policy; the later HAL adapter must retain safe metadata/TOC fallbacks.
- The copied writer still truncates UTF-8 at 65535 **bytes**, potentially splitting
  a code point, and does not enforce the device's 32768-byte per-page payload cap.
  These are inherited producer limitations, not parser memory-safety issues. Do not
  claim every possible writer input yields a device-readable book; future writer
  validation should report unsupported output sizes explicitly.
- Main pipeline OCR transforms use integer/rounded crop coordinates; the current
  regression proves the tested scale/margin case. Additional odd-dimension cases
  will be useful alongside eventual on-device overlay geometry tests.
- Native decoder tests cannot prove HAL file lifecycle, image output, hardware
  allocation behavior, flash headroom or on-device manga support. PDF/YOLO/live OCR
  dependency execution remains outside the offline suite.
- Converter README and both license files are now present and reviewed; they retain
  Dave Allie and Eszter Schuffert MIT attribution, disclose optional dependencies,
  non-atomic/no-resume behavior and current absence of on-device reader support.
  Format documentation now records the binary contract, full field widths,
  optional-file strictness and view lifetimes. A wording correction was requested
  and reviewed as resolved: OCR coordinates are in converted-page space, including
  preserved upstream endpoint/size defects; converter adds the crop/page origin.
  Parent reported final validation: 517/517 native tests, nine converter tests passing
  with one PDF skip, and C3/S3 toolchain object builds with warnings as errors and
  exceptions disabled. These runs were not repeated by this reviewer.

No dictionary, font, reader or existing persistence code was edited by this review.
No Git commit/push or hardware action was performed.
