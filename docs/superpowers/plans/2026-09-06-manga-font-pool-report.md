# Built-in font table pooling report

Completion-plan step 1 implementation, 2026-09-06.

## Change and boundaries

`src/main.cpp:24` includes `builtinFonts/all.h` in the font registration translation
unit. The manifest selects 32 reading fonts (default or noemoji) and five UI
fonts. Their immutable array definitions include repeated Unicode intervals,
kerning class maps, and ligature tables.

`scripts/pool_builtin_fonts.py` reads that manifest and validates all 69 source
headers. It emits copies beneath `$BUILD_DIR/pooled-fonts/builtinFonts`, sharing
arrays only when the C++ type, element count, structure shape, and every literal
integer value match. Content hashes name shared tables, and individual guarded
headers make sharing independent of include order and the default/noemoji branch.
Original array names become preprocessor aliases to actual arrays, preserving
`sizeof`, array extent, indexing, and descriptor pointer expressions without
creating pointer/reference objects or runtime initialization.

`scripts/pool_builtin_fonts_pio.py` runs before dependency scanning and adds
the generated root to `CPPPATH` and as an explicit `-I` in `CCFLAGS`. The latter
is required because PlatformIO prepends library paths to `CPPPATH` after the
pre-script; SCons emits `CCFLAGS` before the resulting `$_CPPINCFLAGS`.
Both hardware and simulator `extra_scripts`
blocks in `platformio.ini` include the hook. The copied `all.h` is byte-identical
to the original, including its conditional selection. Only referenced pool
headers are included in a given variant.

No font source header, font ID, registration, source font, SD font format,
dictionary code, or SDK file/gitlink was changed. No global constant-merging flag
was added. Runtime heap and stack usage are unchanged: the mechanism replaces
repeated immutable static storage with shared immutable static storage. Equality
of addresses for duplicated tables changes intentionally; table contents and
the per-font descriptor values do not.

The parser rejects unsupported static-const declarations, unknown types,
nonliteral expressions, field/extent mismatches, and out-of-range integers before
writing any output. Shared definitions assert their type sizes against the
current `EpdFontData.h` ABI. An output path that would overwrite font sources is
rejected. Future font generator syntax or ABI changes require updating the
parser/tests explicitly. The generator uses only Python's standard library.

Unchanged generated files keep their timestamps. Obsolete content-hash headers
can remain in the build directory but are never included; cleaning that build
directory removes them. `.pio/` output is ignored by Git; no generated copy is
tracked or staged.

## Candidate storage reduction

These are exact duplicate table payload totals from source values and current
type sizes, **not measured linked firmware image savings**. Linker alignment,
garbage collection, and existing compiler decisions can change final savings.

| Table type | Default bytes saved | Noemoji bytes saved |
| --- | ---: | ---: |
| EpdKernClassEntry | 91,245 | 92,142 |
| EpdUnicodeInterval | 34,752 | 26,556 |
| EpdLigaturePair | 496 | 496 |
| int8_t (kerning matrices) | 0 | 50,112 |
| Other types | 0 | 0 |
| Total | **126,493** | **169,306** |

Each selection contains 286 arrays. These totals conservatively require typed
value equality and need not match the earlier approximately 135,533-byte broad
ELF duplicate scan. The build writes the machine-readable calculation to
`$BUILD_DIR/pooled-fonts/font-pool-report.json`.

Reproduce the source report without PlatformIO:

```sh
python3 scripts/pool_builtin_fonts.py --output /tmp/crossink-font-pool-measure
```

## Verification

Host command:

```sh
python3 -m unittest discover -s test/builtin_font_pool -v
```

The tests cover:

- Real-header transformation: expand every alias/shared definition and reconstruct
  every original header byte-for-byte, including unchanged bitmaps, glyph metrics,
  references, and preprocessor statements.
- Real native C++ compilation for both default and noemoji. Original and generated
  include paths produce byte-identical streams of every compiled array, with
  static assertions for every array size, every descriptor scalar and named
  table reference, and null built-in callbacks. This exercises the actual compiler
  and actual generated include hierarchy independently of textual reconstruction.
- Type separation, changed-value separation, numeric spelling normalization,
  fail-closed handling of unsupported syntax and bad values, source-overwrite
  protection, and byte-for-byte reproducibility in different output directories.
- A regression test executes the hook and emulates PlatformIO's later library
  include-path prepend, then compiles a manifest with a nested font include to
  prove both resolve through generated headers. The temporary path contains
  spaces. This test failed with the original hook and passes with explicit
  compiler flag ordering.

Initial tests were run before implementation and failed because the generator
did not exist. The pre-fix five-test run passed on macOS in 28.682 seconds,
including the native C++ comparison and source-overwrite protection.
The suite is registered as `builtin_font_pool` in the native CTest tree.
An isolated CMake configuration and `ctest -R '^builtin_font_pool$' -V` passed:
one CTest entry, five Python tests, 29.94 seconds. Logs are
`/tmp/crossink-font-pool-configure.log` and `/tmp/crossink-font-pool-tests.log`.
`git diff --check` passed. No PlatformIO build was run by this subtask, as assigned;
the parent task owns firmware builds, linked-size measurement, and build include
priority verification. Do not treat the source estimate as a firmware size gate.

The parent's first C3 integration build exposed a hook ordering defect: the
image remained 6,551,008 bytes and `main.cpp.d` still named the original `all.h`.
The hook now emits the generated include path through `CCFLAGS` as described
above. The regression test passes; the parent must rebuild and verify generated
dependency paths and actual linked size before accepting this integration.
After this correction, the full CTest entry passed all six Python tests in
32.70 seconds; `/tmp/crossink-font-pool-tests.log` contains the updated run.

## Remaining integration and hardware acceptance

Build C3 default, Sticky, X4 Pro, and simulator; verify compiler include order
selects the generated root, confirm generated headers participate in dependency
tracking, and measure C3 ELF/image size against the pre-pooling build. Existing
firmware size checks must pass. The native compiler tests pass both font branches;
the parent should include noemoji firmware if that variant is shipped.

On X3/X4, open a book and cycle all Bitter/Lexend Deca sizes and styles; inspect
kerning pairs such as AV/To, fi/fl ligatures, accented text, emoji/symbol/CJK
fallbacks in default, and UI labels rendered with Inter. Repeat font choice and
text checks on available S3 hardware. No EPUB or SD-font cache reset is required
because layout values and formats are identical. Physical rendering checks and
S3 acceptance remain unperformed by this subtask.
