# crossink-manga

Convert CBZ/ZIP, CBR/RAR, EPUB, PDF, or image folders into panel-first books for CrossInk on the Xteink X3, X4, and X4 Pro.

```sh
crossink-manga convert volume.cbz --device x3 --output ./Volume-X3
crossink-manga convert volume.cbz --device x4 --output ./Volume
crossink-manga convert volume.cbz --device x4-pro --output ./Volume-Pro
crossink-manga validate ./Volume
```

Copy the resulting `Volume` folder to the SD card and open it from Books using firmware with the CMI1 version 3 adapter. Each panel is a reading unit. The `x3` profile fits within 528×792; `x4` (the default) and `x4-pro` fit within 480×800. For landscape crops those bounds are swapped before resizing, so wide X4 panels can use up to 800×480 pixels. Updating the firmware is required for these version 3 books; older books remain readable. Output preserves aspect ratio without upscaling, and uses 1-bit BMPs with Floyd–Steinberg dithering by default, plus indexed OCR. Pass `--dither bayer` to reproduce the earlier ordered dithering. Dithering changes only final image export: original crops and OCR remain reusable, and no firmware update is required for this choice. The selected method is recorded in `manifest.json`. Existing Matcha-format books remain supported.

The native backend runs detection and recognition in Rust through ONNX Runtime. It does not invoke uv or Python. It needs the native runtime library and exported model files; these are host-side assets, never copied to the e-reader. Development builds can select them with `--models PATH` and `ORT_DYLIB_PATH`. Release bundles include these assets alongside the executable.

Existing Mokuro JSON can still be imported with `--mokuro` or compared with `compare`. EPUB input follows spine order and selects the first supported image in each wrapper page. PDF input uses the native C++ Poppler `pdfinfo` and `pdftoppm` programs on PATH at 144 DPI; those tools must be installed separately.

## Pipeline and work files

Images are naturally ordered, split using white gutters (top-to-bottom, right-to-left), and cropped at original resolution. Mokuro recognizes those lossless crops **before** device resizing. OCR rectangles are transformed using the actual exported dimensions.

`Volume.work` retains full-resolution crops, numbered previews, `panels.json`, preparation metadata, and OCR results. Native output is `crops.native.mokuro`, with per-crop JSON caches under `native-ocr`; cache identities include crop bytes, model files, the converter executable, and the native runtime. Native progress is printed to stderr. Keep this folder to retry conversion without repeating preparation; source names/content, panel settings, and crop hashes must match. Existing output folders are never overwritten. Export is validated in a temporary sibling folder before publication.

Gutter detection is deliberately basic: borderless, overlapping, or unusual layouts can require correction. A page without detected splits remains a full-page reading unit and produces a warning. OCR accuracy depends on the artwork; the tool does not guarantee perfect recognition.

The device choice affects export only. To reuse existing OCR across devices, pass the same `--work` directory (native results are cached), or explicitly use `--mokuro WORK/crops.native.mokuro` with a new output folder. The final manifest records `device`; preparation remains device-independent. Version 2 readers also accept version 1 books, but old version 1 readers cannot open new exports.

## Review or correct panels

```sh
crossink-manga prepare volume.cbz --output ./Volume
```

Inspect `Volume.work/previews`. Copy `Volume.work/panels.json` to `panels-corrected.json`, then edit ordered rectangles in original-image coordinates:

```json
{"chapter1/001.png": [[600, 0, 1200, 900], [0, 0, 600, 900]]}
```

Each rectangle is `[left, top, right, bottom]`; right/bottom are exclusive. Array order is reading order. Omitted source pages use gutter detection. Use a new work folder when changing the map:

```sh
crossink-manga convert volume.cbz --output ./Volume --panel-map panels-corrected.json --work ./Volume-corrected.work
```

Advanced options include `--title` and `--mokuro /path/to/crops.mokuro`. The latter must describe the prepared crops with matching filenames and dimensions; original whole-page OCR is not interchangeable. This override checks schema and correspondence, but cannot authenticate OCR provenance.

## Build and verification

Build with Rust 1.88 or newer. CBR support additionally needs the separate RAR helper, built with a native C++ compiler:

```sh
cargo +stable build --release --locked
cargo +stable build --release --locked --manifest-path rar-helper/Cargo.toml
cargo +stable test --locked
cargo +stable clippy --locked --all-targets -- -D warnings
```

The executable is `target/release/crossink-manga` (`.exe` on Windows). Place the separately built `crossink-rar` beside it for CBR support. Release packaging uses the Rust `xtask`; see [PACKAGING.md](PACKAGING.md). Model export under `dev/` uses Python only during development; runtime inference is native. UnRAR and other dependencies retain their own included license terms.

The combined native tool is GPL-3.0-only, with upstream notices and corresponding source included in the bundle. The separate RAR helper and original modules retain their stated licenses.

The current firmware consumer reuses the manga reader and dictionary interface. A new OCR-region popup UI and removal of the older implementation are not included. Before replacing it, test the generated folder on a physical X3, X4, and X4 Pro: verify panel order and OCR alignment, open dictionary lookup, turn through the book, sleep/resume, and repeatedly open/close it while checking free heap, largest allocation, and stack headroom. Use a new book folder for this test to avoid existing pixel caches.

The wire format is documented in `docs/mokuro-format.md` in the CrossInk repository.


The complete local 185-page / 431-panel comparison passed: 99.14% region recall, 98.97% precision, and 9.45% whole-panel character disagreement. See [VALIDATION.md](VALIDATION.md) for criteria, evidence and limits.

## Compare native and upstream

Compare an existing reference Mokuro file against a native export of the same prepared crops:

```sh
crossink-manga convert volume.cbz --backend native --models ./models --work ./comparison.work --output ./native-book
crossink-manga compare ./comparison.work/crops.mokuro ./comparison.work/crops.native.mokuro --report ./comparison.json
```

The comparison requires exactly the same page names and dimensions. It reports one-to-one region overlap (IoU >= 0.5), region precision/recall, Unicode character edit distance for matched blocks, and whole-panel text disagreement including unmatched regions and ordering differences. Whitespace is ignored. Edge-crossing rectangles are intersected with the source image for both backends; the report identifies clipped blocks and the original OCR files remain unchanged. Export uses the same clipping rule. Thresholds are 90% region precision/recall and at most 10% matched/whole-panel character disagreement. A nonzero exit status means a threshold failed; the report is still written. Agreement with upstream is not ground-truth OCR accuracy. User manga and OCR contents stay outside the repository.
