import ast
import hashlib
import json
from pathlib import Path
import re


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

