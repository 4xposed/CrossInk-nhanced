# Task 7 review — geometry and grayscale pixel caches

Work remains uncommitted on `matcha_features`; dictionary implementation and SDK
gitlink are unchanged. This records review decisions independently of build results.

- Geometry review caught clockwise auto-rotation where pinned Matcha uses the
  counterclockwise quarter-turn. Fixed through a shared helper with all four
  orientation mappings tested. Geometry suite: 8 passing under ASan/UBSan.
- Cache review added page/panel/source-axis limits, reset of stale paths on failed
  configuration, raw-file sync, cleanup-failure logging, and second-rename failure
  coverage. Cache suite: 8 passing; cover regression suite: 9 passing.
- Ruling: the pixel pair is disposable. Failed publication may lose the old cache,
  but missing files, mismatched identity, bad dimensions/length or CRC cannot expose
  rows to rendering. Regeneration or BW fallback is sufficient. Backup/rollback
  semantics would preserve cache warmth only; they are not added. Reviewer withdrew
  the original publication blocker after checking this contract.
- Ruling: grayscale restores BW by replay into the same framebuffer, followed by
  the existing `cleanupGrayscaleWithFrameBuffer()` API. No 48 KB BW backup is
  allocated. Review confirmed the renderer explicitly supports this use.
- One 9,216-byte fallible scratch allocation is retained for the activity lifetime:
  up to 8,192 raw BMP row bytes plus two 512-byte packed rows. Cached drawing reuses
  it for every plane. Allocation failure keeps the existing BW rendering path.
- BMP review found unchecked allocations in the shared error-diffusion decoder,
  plus insufficient validation of extended headers and pixel offsets. The manga
  path now validates a bounded DIB40/BI_RGB layout before parsing and disables that
  decoder's error diffusion. Tests cover malformed/truncated headers and equivalent
  top-down/bottom-up high-color input.
- Ruling: arbitrary color BMPs use fixed four-level quantization, with simpler
  shading than error diffusion. Converter-produced native-gray/monochrome BMPs
  retain their values. This avoids hidden dither allocations and storage-order
  differences; JPEG/PNG keep their existing dithering path. Extended BMP headers
  outside the supported format are rejected rather than misinterpreted.
- Cached source dimensions are reused only after source path, size and content CRC
  match the validated sidecar. Final pixel opening still validates the complete
  render identity and payload. A monochrome source without a pixel sidecar avoids
  a full fingerprint scan. Native identity lookup tests cover invalid metadata.
- Final review caught a BMP validation bypass after a metadata hit followed by raw
  cache failure. A `bitmapProbed` guard now strictly validates that fallback and
  checks source dimensions before calling `Bitmap`; scoped re-review found no
  remaining blockers or new fallback/decoder/allocation regressions.

Validation results and hardware limitations are recorded in the main manga port
validation ledger. JPEG/PNG decoders remain simulator stubs; a real BMP exercises
pixel replay there, while JPEG/PNG image quality still needs device verification.
