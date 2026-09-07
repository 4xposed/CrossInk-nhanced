# Rust Mokuro converter implementation plan

> **For agentic workers:** Use subagent-driven-development for independent tasks, with task reviews and final review. Never commit or push.

**Goal:** Convert CBZ/CBR archives into a validated X4 book folder using full-resolution panel OCR.

**Architecture:** A Rust CLI owns preparation, resumable work, OCR orchestration and export. A bounded firmware adapter consumes the indexed format using the existing reader infrastructure while a replacement is validated.

**Tech Stack:** Rust stable, image, serde/serde_json, clap, zip, statically built unrar; upstream Mokuro via bundled/PATH uv and an isolated tool environment; native C++/CTest firmware validation.

**Spec:** docs/superpowers/specs/2026-09-07-mokuro-rust-tool-design.md

## Global constraints

- No Git commits/pushes, no deletion of unrelated work. Work on the existing matcha_features branch in focused files; a separate checkout would omit the existing uncommitted manga docs being integrated.
- Full-resolution crops precede OCR and device resize. X4 fits inside 480x800 without upscaling.
- Wire format is docs/mokuro-format.md. Portable metadata is not a promise of hardware validation.
- Stages are internal to `crossink-manga convert`; advanced overrides are allowed for testing/reuse.
- Final output publication requires validation; fail on incomplete OCR, unsafe archives and oversized records.

## Task 1: bounded firmware consumer

Files: lib/MangaPanel/MangaBook.{h,cpp}, new lib/MangaPanel/MokuroBookFormat.{h,cpp} if useful, test/manga_book and native build lists where required.

Interface: preserve MangaBook public API; recognize book.mki as well as panels.idx. Read 12-byte CMI1 header and 20-byte records; adapt book.mkd to PageView without a second full-page buffer. Dimensions and canonical image names follow the format doc. Expose source identity through a small optional API only if required; no large UI redesign.

- [x] Add a synthetic book fixture test that opens CMI1, reads Japanese text, preserves dimensions, and rejects bad extents/flags/rectangles and truncation.
- [x] Run MangaBookTest and record expected failure before implementation.
- [x] Implement validated index/data reads, bounded allocation and format detection. Test malformed input after a previously good read to ensure stale views are cleared.
- [x] Run the focused native suites; inspect other targets that compile MangaBook directly and update source lists if adding a translation unit.

Example independent record header: `CMI1 01 00 00 00 01 00 00 00`, followed by offset=0, length=4, width=480, height=800, originalPage=0, panel=0, reserved=0. Data `00 00 00 00` must yield a valid full-page unit with no OCR.

## Task 2: archive input and managed OCR boundary

Files: tools/crossink-manga/src/input.rs, runtime.rs and module-local tests. Root owns Cargo.toml/main.rs/lib.rs integration.

Interfaces:
```rust
pub fn collect(input: &Path, destination: &Path) -> anyhow::Result<Vec<PathBuf>>;
pub fn run_mokuro(crops: &Path, uv_override: Option<&Path>) -> anyhow::Result<PathBuf>;
```

collect writes safe owned image files into destination, returns deterministic natural relative-path order as absolute paths; supports folders, zip/cbz and rar/cbr. Restrict supported images to JPEG/PNG/WebP/BMP and reject unsupported archive/input types clearly. Reject traversal/symlinks/duplicate normalized names; bound file count, entry bytes and total expansion. Use the unrar memory API after validating size, then write to validated paths.

run_mokuro locates explicit uv, sibling bundled uv, then PATH uv. Run pinned Mokuro 0.2.5 with Python 3.11 through `uv tool run`, no shell, CPU fallback, legacy HTML disabled, confirmation disabled. It must check generated JSON exists and return its path; caller validates per-panel completeness. Error messages identify runtime startup and OCR output failures. Release packaging supplies uv; do not silently claim an unbundled development binary has it.

- [x] Write traversal, repeated basename/order, corrupted archive and missing-runtime tests; verify failure.
- [x] Implement the two boundaries; test ZIP with real bytes and RAR against an actual small fixture if obtainable.
- [x] Run focused cargo tests and report platform/runtime limits.

## Task 3: crop, OCR schema and final exporter

Files: tools/crossink-manga/Cargo.toml, src/{lib,main,panels,mokuro,format,pipeline}.rs, tests/pipeline.rs.

Interfaces: public `convert(input, output, options) -> Result<()>`; options contain optional panel manifest, existing mokuro input, uv executable and work directory. Crop manifest uses relative source paths plus ordered xyxy rectangles. Block JSON uses upstream field names, preserving line metadata in the work directory.

- [x] Add tests for full-resolution crops, gutter splitting/order, independent rectangle transforms and binary literals; establish red.
- [x] Implement deterministic gutters with manual overrides, full-page fallback warnings and numbered preview. Never infer textless panels from OCR.
- [x] Implement safe image loading and sequential preparation. Fingerprint source files and settings; reuse only a matching prepared work directory and valid OCR. Refuse unrelated/inconsistent work.
- [x] Implement source/path/dimension/schema validation, transformed rectangles, bounded output and 1-bit BMP encoding. Preserve raw OCR; export text and vertical flag into book.mkd.
- [x] Validate generated files by reading index, records and images independently; atomically publish a new output directory. Refuse existing output.
- [x] Test end-to-end conversion with a deterministic OCR fixture, oversized records, missing OCR panels, and refusal to overwrite an existing output. Process-kill/power-loss fault injection remains future validation.

## Task 4: packaging, verification and review

Files: tools/crossink-manga/README.md, .github/workflows/manga-tool.yml, CHANGELOG.md, docs/file-formats.md.

- [x] Document one-command conversion, first-run dependency/model downloads, manual panel corrections, resume behavior and current firmware compatibility.
- [x] Add OS-native CI jobs running locked tests/clippy and release builds; include uv plus dependency notices in artifact archives. No release publishing.
- [x] Run Rust fmt/test/clippy/release and focused native C++ tests. Run simulator/default firmware builds for adapter changes.
- [x] Run a real OCR smoke if the environment permits; otherwise explicitly report missing model/runtime evidence and do not call it hardware-ready.
- [x] Review all changes, fix findings, check git status and deliver commands, test evidence and hardware acceptance steps.

## Execution ledger

Approved scope: end-to-end archive conversion, Rust implementation. Ruling: preserve shared firmware image/dictionary infrastructure through an adapter rather than remove working code before X4 validation. Ruling: package uv with release archives so users need not install Python manually; models/dependencies are separate managed downloads. Ruling: native gutter/manual panels first, with explicit fallback warnings, as in the approved spec.

Preflight: task 1 and task 3 share the documented wire format; task 2 supplies collect/run_mokuro to task 3; task 4 consumes all verification results. Tasks 1 and 2 own disjoint files; root owns Cargo integration, task 3 and documentation. All tasks' tests exercise the same limits as their implementation requirements.


## Verification outcome

- Rust: 33 tests passed; locked offline release build, strict all-target/all-feature Clippy, and formatting checks passed.
- Firmware: 82 focused native book/navigation/cover tests passed. Simulator and ESP32-C3 default builds passed. Default flash usage is 99.1%; static RAM usage is 18.9%, not a runtime heap measurement.
- Real Mokuro 0.2.5 CPU smoke: two original-resolution Japanese panel crops exported successfully. A native C++ harness accepted that Rust output and verified exact second-panel Japanese text and 480×360 geometry. First-panel OCR contained recognition errors; this is pipeline evidence, not an accuracy benchmark.
- Independent review found an edge-crop preview panic. Reproduced with a failing test, fixed projection to use actual preview dimensions with clamped endpoints, then verified green.
- Local packaging exposed pre-1980 timestamps in vendored notices; ZIP packaging now clamps unsupported timestamps. Native Linux/Windows/Intel macOS CI and physical X4 acceptance remain unrun.
- Existing Matcha reader infrastructure is retained through the adapter. Replacement region-popup UX and removal of the older implementation are outside this completed converter increment.
- No commits or pushes. Existing unrelated workspace changes were preserved.

- Final macOS ARM64 bundle was built and ZIP contents/notices verified; its packaged executable validated the release output. The release binary also converted a CBZ using managed Mokuro and reused full-resolution work successfully.
