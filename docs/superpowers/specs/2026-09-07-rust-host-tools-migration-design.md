# CrossInk host tools migration to Rust

Status: proposed design for review; implementation has not started.

## Scope and outcome

Migrate the behavior of all CrossInk-owned Python tools and tests to Rust or,
where they directly test firmware C++, existing native C++ test targets.
Leave every file and the gitlink in `freeink-sdk` unchanged. The current
inventory contains 55 CrossInk-owned Python files, including uncommitted
files and three package markers, plus 10 excluded SDK Python files.

Python remains a PlatformIO runtime dependency. CrossInk may retain minimal
SCons adapters that register build actions, pass resolved environment values
to Rust executables, and apply returned build settings. These adapters must
contain no parsers, generators, conversion algorithms, or patch policy.
Do not replace PlatformIO as part of this migration.

## Alternatives and choice

Use a Cargo workspace under `tools/rust/`, containing focused host-tool
packages. Rust offers checked ownership and convenient error propagation for
file-processing utilities. Distribute executables for supported host systems;
source builds require Cargo. Do not couple ordinary firmware builds to PDF,
model inference, font-conversion, or plotting dependencies.

C++ is a viable alternative that shares the firmware language and CMake
toolchain, but is less attractive for independent host utilities with many
structured-data and archive inputs. Use C++ where tests benefit from directly
exercising firmware code. A complete build-system replacement would remove
the Python adapter requirement but substantially broadens scope; exclude it.

## Components and migration order

1. Dictionary tools: replace `scripts/dictionary_tools.py`,
   `scripts/gen_dict_spx.py`, and `tools/dict_convert/convert_jmdict.py` with
   a dictionary CLI and reusable Rust library. Preserve prep, lookup, merge,
   sparse-index generation/validation, and JMdict/Yomitan/MDict support.
2. Build assets: port web compilation/preview, i18n generation, font pooling,
   icon generation/conversion, hyphenation embedding, and font/release
   manifests. Keep generator and preview rendering logic shared.
3. Build orchestration: port firmware naming/version computation, size and
   touch checks, dependency patching, size history, simulator launch/smoke
   harnesses, and unit-test orchestration. Retain only necessary SCons glue.
4. Fonts: port both converters, both font-build wrappers, compression checks,
   and format-version definitions. Preserve FreeType options, fallback order,
   glyph metrics, kerning, variable-font behavior, and binary layouts.
5. Manga: port archive/page collection, ordering, normalization, panel
   detection, OCR requests, metadata, TOC, and binary writing. Preserve PDF
   support and optional model inference. Validate native replacements for
   Pillow, PyMuPDF, Ultralytics, and model retrieval before removing Python;
   wrapping the old Python implementation does not count as migration.
6. Developer utilities and fixtures: port the serial monitor including its
   memory graphs, BMP/EPUB fixture generators, and hyphenation evaluation.
   Preserve graph functionality and the independent hyphenation oracle.
7. Tests: migrate Python test behavior to Rust integration tests or native
   C++ tests, including web portal JavaScript checks and source-boundary
   harnesses. Keep tests against actual production behavior. Remove obsolete
   package markers after the final Python imports disappear.

Each stage updates its consumers, documentation, CI, and changelog before
retiring the corresponding Python code. Existing Python can serve as a
temporary comparison oracle; it is not the final implementation.

## First deliverable: dictionary toolchain

Create `tools/rust/Cargo.toml` and a `crossink-dict` package with separate
modules for StarDict, Japanese dictionary records, sparse indexes, source
formats, and CLI dispatch. Keep format code independent of CLI arguments.
Use subcommands for prep, lookup, merge, conversion, and SPX generation.
Retain current arguments' capabilities and document invocation changes.

The existing SPX implementation at `scripts/gen_dict_spx.py:14` defines
40-byte index records, 32-byte headwords, and a 32-byte version-1 header with
48-record stride. Preserve these bytes exactly. Reuse independently specified
expected bytes from `test/japanese_dict_converter/test_converter.py` rather
than deriving test expectations from the new implementation's constants.
Do not declare dictionary migration complete while MDict support is missing.

## Compatibility and failure handling

Keep file formats, Unicode handling, sorting, and successful CLI behavior
compatible. No cache-version bump is needed merely for changing the host
implementation language. Compare deterministic binary output byte-for-byte;
for archive metadata or compression differences, compare decoded content and
verify the device-facing contract. Image/font changes require pixel or glyph
metric comparisons and device validation, not only successful decoding.

Return useful errors and nonzero exit codes for malformed input and failed
I/O. Validate sizes and offsets before allocation or serialization. Preserve
atomic publication where present, and never overwrite a valid artifact with
partial output. Preserve archive path traversal protections and secret-handling
behavior. Test process arguments, paths with spaces, and failure propagation.

## Build and validation

Commit no Git changes: the repository's global Git policy prohibits commits
and pushes. Preserve the extensive existing staged and unstaged work.

Pin Rust dependencies with a workspace lockfile; choose and document an MSRV
when implementation dependencies are selected. CI must run formatting, Clippy,
unit/integration tests, and the migrated callers. Build tools must be resolved
explicitly with actionable missing-tool errors, without silently installing
toolchains. Bootstrap host build tools once before firmware build actions.

Validate macOS, Linux, and Windows in CI. For build-hook changes, build
`simulator`, `default`, `sticky`, and `x4-pro`; run affected smoke tests.
Do not claim throughput improvements without release-mode measurements.

For the first hardware check, copy Rust-converted dictionaries to an X3/X4
SD card and verify successful loads, expected Japanese and StarDict lookup
results, and definitions matching Python-generated data. Repeat on an S3
device. Later stages verify generated fonts, web pages, and manga on their
relevant device paths. Clear affected book caches only if changed generated
content could otherwise be hidden by cached output.

## Completion criteria

All 55 owned Python files are accounted for by replacements, deletion of
obsolete package markers, or documented minimal PlatformIO adapters. No SDK
changes. No loss of existing optional functionality. CI, documentation, and
developer entry points use the replacements, and each stage has evidence of
behavioral compatibility. Stage-specific implementation plans follow review
of this migration direction.
