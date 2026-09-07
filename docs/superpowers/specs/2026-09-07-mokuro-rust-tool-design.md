# Rust panel-first Mokuro tool

Date: 2026-09-07
Status: Proposed design, updated to require archive-to-X4 end-to-end conversion; written-spec review pending.

## Objective

Ship a desktop command-line tool that prepares manga for the X4 using the agreed pipeline: detect and order panels, crop original-resolution images, run Mokuro on those crops, then resize images and transform OCR geometry. Each panel becomes one Mokuro page. Preserve original page/panel identity for future reader progress and overview navigation.

The user-facing deliverable is one Rust tool that accepts a manga archive (.cbz/.zip or .cbr) and produces the final X4 book folder ready to copy to SD. Full-resolution cropping, OCR and export are internal stages. The matching native firmware reader is required for the end-to-end feature; a generic Mokuro export alone does not satisfy the objective. Build a minimal firmware consumer alongside the final format before calling output device-ready. Replace the existing reader after the X4 prototype demonstrates usable lookup and memory behavior.

## Implementation language and distribution

Recommend Rust for the CLI, image processing, schema validation and export. Use one implementation, not parallel Rust and Go versions. Rust's image crate provides native codecs and image operations; clap provides CLI argument parsing. Pin dependency resolution with Cargo.lock and use stable Rust for release builds.

Alternatives considered:

- Go is also reasonable for a distributable CLI, but offers no inherent solution to shipping Mokuro's Python/model dependency. There is no established Go component in the proposed pipeline requiring it.
- A native OCR reimplementation could eventually remove Python, but would require model conversion and recognition-parity work. Exclude it from the first implementation.

Target downloadable archives for macOS Apple Silicon and Intel, Linux x86-64, and Windows x86-64. Validate on native CI runners before advertising a platform as supported. Creating build workflows is in scope; publishing releases is a separate action. No Git commits or pushes are authorized.

## OCR packaging proposal

Recommend a Rust executable with a separately managed, isolated Mokuro environment. An explicit setup command obtains a pinned Python environment and Mokuro dependencies, leaving the user's system Python untouched. Model files remain an external cache. First setup needs downloads; later conversion should be able to reuse installed dependencies and weights offline.

The alternative under discussion is a platform-specific app archive bundling the Python runtime and dependencies, removing runtime setup at the cost of larger artifacts and additional platform verification. Either choice retains upstream Mokuro inference. The tool must manage this dependency; users should not have to run Python scripts, Mokuro commands or stage-by-stage conversions themselves. Do not describe the entire OCR pipeline as a dependency-free executable.

An optional advanced import path can reuse previously processed panels and matching .mokuro without initializing Python. It is not the primary workflow. Invoke subprocesses using argument arrays, retain diagnostic logs, check exit status and validate all expected OCR results. A successful child-process exit alone is not proof of complete OCR.

## Commands and stages

Proposed primary command: `crossink-manga convert "volume.cbz" --output ./Volume --device x4`. The same command accepts .cbr. It performs the whole pipeline and reports progress and recoverable errors. Internal stages below are implementation boundaries; exposing them as advanced commands is optional.

CBR requires a verified RAR decoder, including a declared RAR4/RAR5 support policy. Do not assume ZIP support covers it. Select and validate a distributable backend before promising a single-executable installation; users should not need to extract CBR manually. Encrypted, corrupt or unsupported archives must fail clearly without publishing a partial book.

- prepare: accept a directory of PNG/JPEG images or CBZ/ZIP/CBR, resolve page order, detect panels, write lossless full-resolution PNG crops and a preparation manifest.
- ocr: run upstream Mokuro against the prepared crop directory. Reuse cached results only when their input identity matches.
- export: validate the prepared crops and .mokuro, fit images into 480x800 without distortion or upscaling, and write transformed Mokuro plus device images and a CrossInk manifest.
- convert: run prepare, ocr and export sequentially, recording stage completion for resumable work.
- validate: check an exported book's image identity, dimensions, bounds and manifest correspondence.

These are proposed commands, not existing functionality. Keep a CLI-first interface; a graphical wrapper is outside this first deliverable.

## Panel detection and review

Separate detection from cropping and OCR. Start with a deterministic native gutter detector plus explicit panel rectangles/order supplied through a manifest. Include a full-page fallback for layouts the heuristic cannot segment, clearly reported rather than presented as successful fine-grained detection. White-gutter detection is not expected to handle all irregular manga pages.

Write a preview contact sheet showing numbered rectangles before OCR so incorrect crops and reading order are reviewable. Preserve textless panels. Crop margins must be clamped to the source image and remain part of the crop identity. Detecting on a reduced analysis image is permitted, but map rectangles back to source coordinates and crop the original image before any OCR.

Retain an interface for a model-based detector, but do not silently download or load a detector based on whatever packages happen to be installed. Native model inference is a later, separately validated enhancement. The first release must clearly describe its heuristic/manual segmentation limit.

## Data contract

Preserve the original crop images and Mokuro JSON as immutable OCR masters. Put resized images and rewritten JSON in a separate export directory. Keep upstream Mokuro fields intact wherever possible, including UUIDs, line strings, vertical direction and line polygons. Do not replace the upstream version field with a CrossInk schema version.

A separately versioned CrossInk manifest associates each exported panel with its original relative image path, original page ordinal, panel ordinal, crop rectangle and source dimensions. Record source hashes, preparation settings and stage/tool versions to invalidate stale work deterministically. Use canonical zero-padded panel filenames, preserve explicit order, and never match OCR to images by independent array order.

For every exported crop, calculate scale factors from actual decoded source and actual output dimensions. Transform block endpoints, all line-polygon points and font size consistently. With aspect-preserving scaling, use the nominal uniform scale for font size and the actual per-axis ratios for coordinates. Validate finite values and positive dimensions; use outward rounding for rectangular bounds. Retain text exactly as OCR produced it. Lookup-specific line joining belongs in the consumer, not destructive rewriting of the master.

The final X4 folder contains device-sized panel images, a versioned book manifest, and indexed compact OCR records that the matching firmware can read one panel at a time. Define exact binary layout, limits and version handling with the minimal C3 consumer before implementing serialization. Full Mokuro JSON is an intermediate/master representation; the X4 must not need to parse a volume-wide JSON document. Keep original-page and panel identities in the final format. Do not lock the output to the Matcha format merely for compatibility, and do not claim device readiness until the matching reader consumes it successfully.

## Robustness

Reject archive traversal, duplicate normalized paths, missing images, mismatched OCR dimensions and unsupported schema shapes with contextual errors. Apply bounded archive expansion and image-decoder limits. Process images sequentially initially to bound host peak memory. Never silently discard a panel or OCR block.

Write stages into temporary locations, validate them, and only then mark them complete. Refuse overwriting unrelated output. Resume only completed stages whose source hashes and relevant settings match. Preserve expensive OCR results across export-only changes.

## Verification

Use deterministic synthetic images for panel ordering, crop-margin clipping, scaling, alpha handling and no-upscale behavior. Test vertical and horizontal line polygons, Unicode, repeated filenames in separate chapters, malformed JSON and interrupted stages. Use independent expected transforms, not expectations calculated by the implementation under test.

Use a fake OCR executable for process failure/incomplete-output tests; a separate real Mokuro smoke test is required before claiming end-to-end OCR support. Test crops where bubbles cross boundaries and layouts with no gutters. Validate exported files using a fresh read of images and JSON.

Run cargo fmt, cargo test and cargo clippy with warnings denied. Build release binaries on each advertised OS. On the eventual X4 consumer, verify panel navigation, region selection, dictionary lookup, clippings, rotation and sleep/resume; measure free heap, largest allocation and task stack headroom. Host tests alone establish neither C3 memory sufficiency nor e-ink quality.

## Existing code and sources

- tools/manga_convert/convert_manga.py:237: current input collection and archive handling.
- tools/manga_convert/convert_manga.py:625: existing gutter-detection approach for comparison.
- tools/manga_convert/convert_manga.py:700: current optional detector dispatch.
- tools/manga_convert/convert_manga.py:1248: current device fitting policy.
- tools/manga_convert/convert_manga.py:1400: current resize-before-detection pipeline; the new OCR path must instead use full-resolution crops.
- src/activities/reader/MangaReaderActivity.cpp:663: current stored-OCR lookup entry point for later firmware integration.
- https://docs.rs/image/latest/image/ : native image operations, checked 2026-09-07.
- https://docs.rs/clap/latest/clap/ : CLI implementation, checked 2026-09-07.
- https://raw.githubusercontent.com/kha-white/mokuro/v0.2.5/pyproject.toml : upstream runtime dependencies, checked 2026-09-07.
- https://docs.astral.sh/uv/guides/install-python/ : managed Python distribution option, checked 2026-09-07. A downloader/runtime choice requires platform smoke verification before inclusion in release claims.

Self-review: the primary workflow is archive-to-final-X4-folder, including managed OCR and CBR support. Internal stages are not required user commands. Full Mokuro JSON remains the source representation and bounded indexed records are the firmware representation. The exact wire format and CBR/runtime packaging must be resolved in the implementation design before code. No implementation or release availability is claimed.
