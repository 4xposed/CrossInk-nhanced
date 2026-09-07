# Native OCR validation — 2026-09-08

The native Rust backend passed the comparison against unmodified Mokuro 0.2.5 on the complete local `Kawazuya v01.cbz`: 185 source images prepared into the same 431 original-resolution panel crops. No manga images or OCR contents are stored in this repository.

| Measure | Result | Required |
| --- | ---: | ---: |
| Identical panel names and dimensions | 431 / 431 | 100% |
| Region recall, IoU ≥ 0.5 | 99.14% | ≥90% |
| Region precision, IoU ≥ 0.5 | 98.97% | ≥90% |
| Matched-region character disagreement | 8.93% | ≤10% |
| Whole-panel character disagreement | 9.45% | ≤10% |

There are 579 reference regions, 580 native regions and 574 one-to-one matches. Whole-panel disagreement is 477 Unicode character edits over 5,049 reference characters. Only whitespace is removed; unmatched regions and ordering differences count. Thresholds were set before the comparison and retained through failed diagnostic runs. These numbers measure agreement with upstream, not ground-truth OCR accuracy or generalization to other manga.

Two upstream rectangles cross the image edge. Comparison and device export intersect rectangles with the visible source image, record the clipping indexes, and preserve both raw OCR masters. Invalid/nonfinite or fully invisible rectangles still fail validation.

The recognizer also matched Python exactly on 43 identical difficult line crops containing 389 reference characters. Native preprocessing uses Pillow-compatible fixed-point resizing; the installed Transformers processor uses Torchvision and can differ by one grayscale level. Detector contour geometry, refined-mask approximation and a one-pixel-per-contour tolerance in split intersection remain intentional sources of non-identical results.

Model inference uses the existing trained detector and manga-ocr weights, exported to ONNX. Native conversion invokes neither Python nor uv. Development-only Python exports and numerically checks the models and produces the upstream reference. The model bundle records source revisions, dependency versions, export-script hashes and model hashes.

Reference environment: Mokuro 0.2.5, manga-ocr 0.1.16, Torch 2.14.0, Transformers 5.16.1, OpenCV 5.0.0.93, Pillow 12.3.0, NumPy 2.4.6. Native acceptance used ONNX Runtime 1.29.0 on macOS ARM64.

Fixture SHA-256: `4194e0032e747baf01873dcae1c9f7782fc62299fb7be7fdc781f697d43c913d`.

Unmodified reference OCR SHA-256: `7273c91e98fa7d07d156ccdf45d5e324b39d3f8636a0d838c8d08b88bde1995b`.

Native OCR SHA-256: `c1221432de749fc66eeac724a2d4cb7c6afe876a167b2a43575874c2bb6daefc`.

## Verification and limits

Regression tests cover archive safety, device records, coordinate transforms, matching, detector grouping/scoring/crops, beam search, preprocessing and cache identity. Independent fixtures establish little-endian CMI records, OpenCV perspective sampling and Pillow resize behavior. Real-model integration and complete-book conversion supplement those unit tests.

The firmware adapter was previously checked with 35 MangaBook native tests and successful C3/default and X4 Pro builds. This host-only OCR increment does not alter device inference or add models to firmware. Physical X3/X4/X4 Pro checks remain necessary: panel order and text alignment, dictionary lookup, page turns, sleep/resume and repeated open/close with heap/stack logs. Test under a new book folder to avoid old pixel caches.

Windows, Linux and Intel macOS builds are configured in CI but were not executed in this local validation. ORT 1.22 aborts on macOS during process teardown; the Intel matrix uses 1.23.2, whose official Intel wheel exists and whose ARM64 runtime completed local inference and teardown successfully.
