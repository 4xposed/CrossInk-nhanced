# Pinned Matcha converter goldens

These binary files freeze the byte-level compatibility target for the future
CrossInk Japanese dictionary converter.  They were captured from Matcha Reader
commit `61ca61ba86e3c5709a24d1b9c4f3cf2d41488012`; do not refresh them when
`git -C ../matcha-reader rev-parse HEAD` prints a different SHA.  Record that
SHA and stop for review instead.

`mini_jmdict` covers ichidan and godan verbs, reading-record collision,
duplicate headwords with different priorities, proper names, grammar entries,
and the accepted 31-byte UTF-8 headword.  `mini_yomitan` covers structured
content flattening, tags, lists, ruby text, image suppression, and list-form
definitions.

Capture procedure (from the CrossInk repository root):

```bash
git -C ../matcha-reader rev-parse HEAD
python3 ../matcha-reader/tools/dict_convert/convert_jmdict.py --help
TASK1_TMP=$(mktemp -d /private/tmp/crossink-task1.XXXXXX)
mkdir -p "$TASK1_TMP/jmdict" "$TASK1_TMP/yomitan"
python3 ../matcha-reader/tools/dict_convert/convert_jmdict.py --input test/japanese_dict_converter/fixtures/mini_jmdict.json --output-dir "$TASK1_TMP/jmdict" --name vocab
python3 ../matcha-reader/scripts/gen_dict_spx.py "$TASK1_TMP/jmdict"
python3 - "$TASK1_TMP/mini_yomitan.zip" <<'PY'
import sys
import zipfile
from pathlib import Path

source = Path("test/japanese_dict_converter/fixtures/mini_yomitan")
with zipfile.ZipFile(sys.argv[1], "w", compression=zipfile.ZIP_DEFLATED) as archive:
    for item in sorted(source.iterdir()):
        archive.write(item, item.name)
PY
python3 ../matcha-reader/tools/dict_convert/convert_jmdict.py --input "$TASK1_TMP/mini_yomitan.zip" --output-dir "$TASK1_TMP/yomitan" --name vocab
python3 ../matcha-reader/scripts/gen_dict_spx.py "$TASK1_TMP/yomitan"
```

SHA-256 values:

| Fixture | File | SHA-256 |
| --- | --- | --- |
| `mini_jmdict` | `vocab.idx` | `72d0afa000ef135174af899af89fbcc6b8276320cd5092542dbccce989b95286` |
| `mini_jmdict` | `vocab.dat` | `6be5e52c89e58b0a68886a084faffd17df793f11683bac8f9cf7d41205bae8aa` |
| `mini_jmdict` | `vocab.spx` | `1114a19da7b3ae9b13db431c902efbbb380fb2d84ffae3329b9e0f1abb750796` |
| `mini_yomitan` | `vocab.idx` | `1d964f719febdf2f415b507269298514947bec763d6e9593c2580ae5d343923a` |
| `mini_yomitan` | `vocab.dat` | `a25323c63e6d802aae8b02a5c8b4068eef88d3cb17a0047d2883f16a19c5cce4` |
| `mini_yomitan` | `vocab.spx` | `fb339ba539389585dd3e767bcf2ca5fe2550facdf907910950a2f86086c02616` |

The index records use `<32sIHBB` (40 bytes).  The `.spx` files use Matcha
version 1 (`CPSPX1`, 32-byte header, 48-record stride).

The related semantic fixture `test/japanese_dictionary/fixtures/parity_cases.json`
uses `expected_status: "not_found"` with zero/empty result fields for a required
surface that Matcha does not resolve.  This preserves one schema for successful
and unsuccessful lookup cases without inventing a fallback result.
