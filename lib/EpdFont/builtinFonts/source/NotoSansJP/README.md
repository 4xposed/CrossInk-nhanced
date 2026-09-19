# Bundled Japanese fallback font

`NotoSansJP-Regular.ttf` is a regular-weight (400) subset of Adobe/Google's
Noto Sans CJK JP variable TrueType font. The upstream font carries the
Adobe 2014–2021 copyright and SIL Open Font License in `OFL.txt`.
`source-sha256.txt` identifies the original variable-font input.

`codepoints.txt` is the explicit, sorted coverage input: 4,183 mapped Unicode
characters from Matcha Reader's Joyo/Jinmeiyo fallback intervals (coverage
introduced at Matcha commit `22f73077`). Matcha's nominal 4,184 glyph slots
include an unmapped slot. The list also includes kana, CJK punctuation,
fullwidth forms and Latin characters; it is not full Unicode coverage.

Rebuild the headers from this checked-in subset, from the repository root:

```sh
python3 -m pip install fonttools==4.65.0 freetype-py==2.5.1 zopfli==0.4.3
cargo run --quiet --locked --manifest-path lib/EpdFont/scripts/Cargo.toml --bin build-japanese-subsets --
```

The Rust script requires Cargo and uses the existing Python fontconvert tool with 2-bit antialiasing,
compression, explicit intervals and no implicit default intervals. To
recreate the pinned subset from the original variable input, pass
`--source /path/to/NotoSansCJKjp-VF.ttf` after the command's final `--`.
The optional source preparation still uses fontTools for instancing and subsetting.
Set `PYTHON=/path/to/venv/bin/python` to select a Python environment; otherwise
the script uses `python3` from `PATH`. Font rasterization also depends on
the FreeType version installed on the host.

Japanese UI translations reuse compatible strings from Matcha Reader;
CrossInk's additional keys and changed source strings were translated
separately. `test/builtin_font_pool/test_japanese_subset.py` checks the
complete UI key set, placeholders and coverage against these inputs.
