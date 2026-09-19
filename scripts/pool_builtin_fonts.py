"""Deterministic built-in font table pooling; originals remain the source of truth.

Only literal, immutable arrays emitted by fontconvert.py are accepted. Generated
headers live beneath an environment's build directory and retain the original
names as preprocessor aliases (no pointer/reference objects or runtime setup).
No compiler constant-merging flags, dependencies, or SD font changes are needed.
"""
import argparse
import ast
from collections import Counter
import hashlib
import json
from pathlib import Path
import re


# Field widths/signs follow EpdFontData.h. Sizes include C++ structure padding;
# pooled definitions assert the ABI instead of silently trusting this report.
TYPES = {
    "uint8_t": (1, ((8, False),)),
    "int8_t": (1, ((8, True),)),
    "uint16_t": (2, ((16, False),)),
    "EpdGlyph": (16, ((8, False), (8, False), (16, False), (16, True),
                      (16, True), (16, False), (32, False))),
    "EpdFontGroup": (20, ((32, False), (32, False), (32, False), (16, False), (32, False))),
    "EpdUnicodeInterval": (12, ((32, False),) * 3),
    "EpdKernClassEntry": (3, ((16, False), (8, False))),
    "EpdLigaturePair": (8, ((32, False),) * 2),
}
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
ARRAY = re.compile(r"static\s+const\s+(\w+)\s+(\w+)\s*\[\s*(\d*)\s*\]\s*=\s*(\{.*?\})\s*;", re.S)


def parse_arrays(source):
    """Return validated literal arrays with exact source spans for replacement.

    Reject unknown declarations, expressions, dimensions, types, field counts,
    and out-of-range values. Comments are blanked without changing offsets.
    """
    clean = COMMENT.sub(lambda match: " " * len(match[0]), source)
    arrays = []
    for match in ARRAY.finditer(clean):
        kind, name, extent, body = match.groups()
        if kind not in TYPES:
            raise ValueError(f"Unsupported table type: {kind} {name}")
        residue = re.sub(r"-?(?:0[xX][0-9a-fA-F]+|[0-9]+)|[{},\s]", "", body)
        if residue:
            raise ValueError(f"Nonliteral initializer: {name}")
        try:
            values = ast.literal_eval(body.replace("{", "[").replace("}", "]"))
        except (ValueError, SyntaxError) as exc:
            raise ValueError(f"Unsupported initializer: {name}") from exc
        size, fields = TYPES[kind]
        structured = kind.startswith("Epd")
        if not values or (extent and int(extent) != len(values)):
            raise ValueError(f"Invalid extent: {name}")
        for value in values:
            row = value if structured else [value]
            if not isinstance(row, list) or len(row) != len(fields):
                raise ValueError(f"Invalid field count: {name}")
            for number, (bits, signed) in zip(row, fields):
                low = -(1 << (bits - 1)) if signed else 0
                high = (1 << (bits - int(signed))) - 1
                if type(number) is not int or not low <= number <= high:
                    raise ValueError(f"Invalid field value: {name}")
        frozen = tuple(tuple(row) for row in values) if structured else tuple(values)
        arrays.append({"name": name, "type": kind, "key": (kind, frozen),
                       "start": match.start(), "end": match.end(),
                       "declaration": source[match.start():match.end()],
                       "bytes": len(values) * size})
    remainder = clean
    for array in reversed(arrays):
        remainder = remainder[:array["start"]] + remainder[array["end"]:]
    if re.search(r"\bstatic\s+const\b", remainder):
        raise ValueError("Unsupported static const declaration in font header")
    return arrays


def font_paths(source):
    """Read only the explicit font manifest, preserving both selection branches."""
    manifest = (source / "all.h").read_text()
    reading = re.findall(r"^#include BUILTIN_READING_FONT_HEADER\((\w+)\)$", manifest, re.M)
    ui = re.findall(r"^#include <builtinFonts/(\w+\.h)>$", manifest, re.M)
    if not (reading or ui) or len(reading) + len(ui) != len(re.findall(r"^#include\b", manifest, re.M)):
        raise ValueError("Unsupported built-in font manifest")
    return sorted([Path(name + ".h") for name in reading]
                  + [Path("noemoji") / (name + ".h") for name in reading]
                  + [Path(name) for name in ui])


def pool_symbol(key):
    digest = hashlib.sha256(json.dumps(key, separators=(",", ":")).encode()).hexdigest()
    return "crossink_font_pool_" + digest


def alias_text(name, symbol):
    return f"#include <builtinFonts/pool/{symbol}.h>\n#define {name} {symbol}"


def write_changed(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def generate(source, output):
    source, output = Path(source), Path(output)
    destination = (output / "builtinFonts").resolve()
    if (destination == source.resolve() or source.resolve() in destination.parents
            or destination in source.resolve().parents):
        raise ValueError("Generated output must be outside source font directory")
    headers = {}
    groups = {}
    # Validate everything before writing output. A parse failure aborts the build.
    for relative in font_paths(source):
        text = (source / relative).read_text()
        arrays = parse_arrays(text)
        headers[relative] = (text, arrays)
        for array in arrays:
            groups.setdefault(array["key"], []).append(array)
    shared = {key: entries for key, entries in groups.items() if len(entries) > 1}
    for key, entries in shared.items():
        first = entries[0]
        symbol = pool_symbol(key)
        declaration = re.sub(r"\b" + re.escape(first["name"]) + r"\b", symbol, first["declaration"], count=1)
        header = ("#pragma once\n#include <EpdFontData.h>\n"
                  f"static_assert(sizeof({first['type']}) == {TYPES[first['type']][0]}, \"Font table ABI changed\");\n"
                  + declaration + "\n")
        write_changed(output / "builtinFonts/pool" / (symbol + ".h"), header)
    for relative, (text, arrays) in headers.items():
        for array in reversed(arrays):
            if array["key"] in shared:
                text = (text[:array["start"]] + alias_text(array["name"], pool_symbol(array["key"]))
                        + text[array["end"]:])
        write_changed(output / "builtinFonts" / relative, text)
    write_changed(output / "builtinFonts/all.h", (source / "all.h").read_text())
    report = {}
    for variant in ("default", "noemoji"):
        selected = [array for path, (_, arrays) in headers.items()
                    if ((Path("noemoji") / path.name not in headers)
                        or (("noemoji" in path.parts) == (variant == "noemoji")))
                    for array in arrays]
        counts = Counter(array["key"] for array in selected)
        savings = Counter()
        for key, count in counts.items():
            savings[key[0]] += (count - 1) * groups[key][0]["bytes"]
        report[variant] = {"array_count": len(selected), "candidate_bytes_saved": sum(savings.values()),
                           "savings_by_type": dict(sorted(savings.items()))}
    write_changed(output / "font-pool-report.json", json.dumps(report, indent=2, sort_keys=True) + "\n")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("lib/EpdFont/builtinFonts"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(generate(args.source, args.output), indent=2, sort_keys=True))
