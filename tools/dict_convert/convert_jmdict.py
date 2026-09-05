#!/usr/bin/env python3
"""Convert offline Japanese dictionary inputs into Matcha-compatible files."""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.gen_dict_spx import (  # noqa: E402 - supports direct CLI execution.
    HEADWORD_SIZE,
    SPX_HEADER_SIZE,
    SPX_MAGIC,
    SPX_STRIDE,
    SPX_VERSION,
    build_spx_bytes,
    validate_spx,
)


JMDICT_URL = "https://github.com/scriptin/jmdict-simplified/releases/latest/download/jmdict-eng-3.5.0.json.tgz"
RECORD = struct.Struct("<32sIHBB")
RECORD_FORMAT = RECORD.format
UINT16_MAX = 0xFFFF
POS_V1 = 0x01
POS_V5 = 0x02
POS_VS = 0x04
POS_VK = 0x08
POS_ADJ_I = 0x10
POS_OTHER = 0x20
POS_READING = 0x40
POS_ANY_VERB = POS_V1 | POS_V5 | POS_VS | POS_VK


def strip_html(html: str) -> str:
    text = re.sub(r"<br\s*/?>", "\n", html, flags=re.IGNORECASE)
    text = re.sub(r"<[^>]+>", "", text)
    for entity, replacement in (("&amp;", "&"), ("&lt;", "<"), ("&gt;", ">"), ("&quot;", '"'), ("&nbsp;", " "), ("&#x27;", "'"), ("&#39;", "'")):
        text = text.replace(entity, replacement)
    return "\n".join(line for line in (line.strip() for line in text.split("\n")) if line)


def pos_flags_from_tags(tags) -> int:
    flags = 0
    for tag in tags:
        if not tag or tag in ("vt", "vi", "aux", "aux-adj", "exp"):
            continue
        if tag.startswith("v1"):
            flags |= POS_V1
        elif tag.startswith(("v5", "v4", "iv")):
            flags |= POS_V5
        elif tag.startswith("vs"):
            flags |= POS_VS
        elif tag.startswith("vk"):
            flags |= POS_VK
        elif tag.startswith("adj-i"):
            flags |= POS_ADJ_I
        elif tag.startswith("v") or tag == "aux-v":
            flags |= POS_ANY_VERB
        else:
            flags |= POS_OTHER
    return flags


def _validate_idx_dat(idx_bytes: bytes, dat_bytes: bytes) -> None:
    if len(idx_bytes) % RECORD.size:
        raise ValueError("idx size is not record-size divisible")
    previous = None
    for record_number in range(len(idx_bytes) // RECORD.size):
        headword, offset, length, _priority, _flags = RECORD.unpack_from(idx_bytes, record_number * RECORD.size)
        nul = headword.find(b"\0")
        if nul < 0 or any(headword[nul:]):
            raise ValueError(f"idx record {record_number} lacks NUL padding")
        key = headword[:nul]
        if previous is not None and key < previous:
            raise ValueError(f"idx record {record_number} is out of order")
        if offset + length > len(dat_bytes):
            raise ValueError(f"idx record {record_number} exceeds dat")
        previous = key


def _temp_paths(output_dir: Path, name: str) -> dict[str, Path]:
    return {suffix: output_dir / f"{name}.{suffix}.tmp" for suffix in ("idx", "dat", "spx")}


def _publish_transactional(output_dir: Path, name: str, payloads: dict[str, bytes]) -> None:
    """Publish a validated three-file set, restoring a prior set on replacement failure."""
    output_dir.mkdir(parents=True, exist_ok=True)
    temporary = _temp_paths(output_dir, name)
    if any(path.exists() for path in temporary.values()):
        stale = next(path for path in temporary.values() if path.exists())
        raise FileExistsError(f"stale temporary output exists: {stale}")
    created: list[Path] = []
    finals = {suffix: output_dir / f"{name}.{suffix}" for suffix in temporary}
    backups = {suffix: output_dir / f"{name}.{suffix}.bak" for suffix in temporary}
    if any(path.exists() for path in backups.values()):
        backup = next(path for path in backups.values() if path.exists())
        raise FileExistsError(f"stale publication backup exists: {backup}")
    originals = {suffix: final.exists() for suffix, final in finals.items()}
    published: set[str] = set()
    try:
        for suffix, temporary_path in temporary.items():
            with temporary_path.open("xb") as output:
                # Exclusive create succeeded, so this invocation owns cleanup.
                # Record it before write() because write errors leave the file.
                created.append(temporary_path)
                output.write(payloads[suffix])
        idx_bytes = temporary["idx"].read_bytes()
        dat_bytes = temporary["dat"].read_bytes()
        spx_bytes = temporary["spx"].read_bytes()
        _validate_idx_dat(idx_bytes, dat_bytes)
        validate_spx(idx_bytes, spx_bytes)

        for suffix, final in finals.items():
            if originals[suffix]:
                os.replace(final, backups[suffix])
        for suffix, temporary_path in temporary.items():
            os.replace(temporary_path, finals[suffix])
            created.remove(temporary_path)
            published.add(suffix)
        _validate_idx_dat(finals["idx"].read_bytes(), finals["dat"].read_bytes())
        validate_spx(finals["idx"].read_bytes(), finals["spx"].read_bytes())
    except Exception:
        for suffix, final in finals.items():
            if suffix in published and final.exists():
                final.unlink()
        for suffix, backup in backups.items():
            if backup.exists():
                os.replace(backup, finals[suffix])
        for temporary_path in created:
            if temporary_path.exists():
                temporary_path.unlink()
        raise

    # Publication is complete above. Backup deletion is maintenance only; a
    # failure must not roll back a complete new set after another backup was
    # already deleted. Leave the backup for an operator to inspect/remove.
    for backup in backups.values():
        if backup.exists():
            try:
                backup.unlink()
            except OSError as error:
                print(f"WARNING: published output; retained backup {backup}: {error}", file=sys.stderr)


def write_binary(records: list[tuple[bytes, bytes, int, int]], output_dir: str | Path, name: str = "vocab") -> tuple[int, int]:
    """Sort records and atomically publish the idx/dat/spx sibling set."""
    records.sort(key=lambda record: record[0])
    dat = bytearray()
    idx = bytearray()
    previous_definition: bytes | None = None
    previous_offset = 0
    previous_length = 0
    skipped_headwords = 0
    truncated_definitions = 0
    for headword, definition, priority, pos_flags in records:
        if len(headword) >= HEADWORD_SIZE:
            skipped_headwords += 1
            continue
        if definition == previous_definition:
            offset, length = previous_offset, previous_length
        else:
            if len(definition) > UINT16_MAX:
                truncated_definitions += 1
            stored_definition = definition[:UINT16_MAX]
            offset = len(dat)
            length = len(stored_definition)
            dat.extend(stored_definition)
            previous_definition = stored_definition
            previous_offset, previous_length = offset, length
        padded = headword + b"\0" * (HEADWORD_SIZE - len(headword))
        idx.extend(RECORD.pack(padded, offset, length, priority, pos_flags))
    idx_bytes = bytes(idx)
    dat_bytes = bytes(dat)
    _validate_idx_dat(idx_bytes, dat_bytes)
    spx_bytes = build_spx_bytes(idx_bytes)
    validate_spx(idx_bytes, spx_bytes)
    _publish_transactional(Path(output_dir), name, {"idx": idx_bytes, "dat": dat_bytes, "spx": spx_bytes})
    print(f"Output:\n  {Path(output_dir) / (name + '.idx')}: {len(idx_bytes):,} bytes ({len(idx_bytes) // RECORD.size:,} records)\n  {Path(output_dir) / (name + '.dat')}: {len(dat_bytes):,} bytes\n  {Path(output_dir) / (name + '.spx')}: {len(spx_bytes):,} bytes (stride={SPX_STRIDE})")
    if skipped_headwords:
        print(f"Skipped {skipped_headwords} headword{'s' if skipped_headwords != 1 else ''} >= {HEADWORD_SIZE} bytes")
    if truncated_definitions:
        print(f"Truncated {truncated_definitions} definition{'s' if truncated_definitions != 1 else ''} > {UINT16_MAX} bytes")
    return skipped_headwords, truncated_definitions


def compute_priority_jmdict(entry: dict) -> int:
    return 200 if any(item.get("common", False) for item in entry.get("kanji", []) + entry.get("kana", [])) else 100


def pos_flags_jmdict(entry: dict) -> int:
    return pos_flags_from_tags(tag for sense in entry.get("sense", []) for tag in sense.get("partOfSpeech", []))


def format_definition_jmdict(entry: dict) -> str:
    parts = []
    readings = [kana["text"] for kana in entry.get("kana", [])]
    if readings:
        parts.append("【" + "、".join(readings[:3]) + "】")
    senses = entry.get("sense", [])
    for index, sense in enumerate(senses[:3]):
        glosses = [gloss["text"] for gloss in sense.get("gloss", [])]
        if glosses:
            parts.append((f"{index + 1}. " if len(senses) > 1 else "") + "; ".join(glosses[:4]))
    return "\n".join(parts)


def _load_jmdict(path: str | Path) -> dict:
    source = Path(path)
    try:
        if source.name.lower().endswith((".tgz", ".tar.gz")):
            with tarfile.open(source, "r:gz") as archive:
                member = next((item for item in archive.getmembers() if item.name.endswith(".json")), None)
                if member is None:
                    raise ValueError("archive contains no JSON file")
                extracted = archive.extractfile(member)
                if extracted is None:
                    raise ValueError("archive JSON cannot be read")
                return json.load(extracted)
        with source.open("r", encoding="utf-8") as input_file:
            return json.load(input_file)
    except (OSError, tarfile.TarError, json.JSONDecodeError, UnicodeDecodeError) as error:
        raise ValueError(f"Invalid JMdict JSON: {source}: {error}") from error


def convert_jmdict(json_path: str, output_dir: str, name: str = "vocab") -> None:
    print(f"Loading {json_path}...")
    data = _load_jmdict(json_path)
    words = data.get("words", [])
    if not isinstance(words, list):
        raise ValueError("Invalid JMdict JSON: words must be a list")
    print(f"Processing {len(words)} JMdict entries...")
    records = []
    skipped_headwords = 0
    for entry in words:
        definition = format_definition_jmdict(entry).encode("utf-8")
        priority = compute_priority_jmdict(entry)
        flags = pos_flags_jmdict(entry)
        seen = set()
        for kanji in entry.get("kanji", []):
            headword = kanji["text"].encode("utf-8")
            if len(headword) >= HEADWORD_SIZE:
                skipped_headwords += 1
            elif headword not in seen:
                seen.add(headword)
                records.append((headword, definition, priority, flags))
        kana_flags = flags | (POS_READING if entry.get("kanji") else 0)
        for kana in entry.get("kana", []):
            headword = kana["text"].encode("utf-8")
            if len(headword) >= HEADWORD_SIZE:
                skipped_headwords += 1
            elif headword not in seen:
                seen.add(headword)
                records.append((headword, definition, priority, kana_flags))
    print(f"Generated {len(records)} index records")
    write_binary(records, output_dir, name)
    if skipped_headwords:
        print(f"Skipped {skipped_headwords} headword{'s' if skipped_headwords != 1 else ''} >= {HEADWORD_SIZE} bytes")


def flatten_structured_content(content) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "".join(flatten_structured_content(item) for item in content)
    if not isinstance(content, dict):
        return str(content)
    if content.get("type") == "text":
        return content.get("text", "")
    if content.get("type") == "image":
        return ""
    if content.get("type") == "structured-content":
        return flatten_structured_content(content.get("content", ""))
    tag = content.get("tag", "")
    data = content.get("data", {}) if isinstance(content.get("data"), dict) else {}
    kind, css_class = data.get("content", ""), data.get("class", "")
    text = flatten_structured_content(content.get("content", ""))
    if tag == "br": return "\n"
    if tag == "rt": return ""
    if css_class == "tag" and kind in ("part-of-speech-info", "field-info", "misc-info", "dialect-info", "language-info"): return "[" + text + "] "
    if css_class == "tag" and kind == "forms-label": return ""
    if kind == "glossary" and tag == "ul": return "\n" + text
    if tag == "li" and not kind: return "• " + text.strip() + "\n"
    if kind == "sense-group" and tag in ("li", "div"): return text + "\n"
    if kind == "sense" and tag == "li": return text
    if kind == "sense-note-label": return text + ": "
    if kind == "sense-note-content": return text + "\n"
    if kind == "sense-note" and css_class == "extra-box": return "  → " + text
    if kind in ("example-sentence-a", "example-sentence-b"): return text + "\n"
    if kind == "example-sentence" and css_class == "extra-box": return "  " + text
    if kind == "xref" and css_class == "extra-box": return ""
    if kind == "reference-label": return text + " "
    if kind in ("forms", "attribution-footnote"): return ""
    return text


def format_definition_yomitan(headword: str, reading: str, definitions) -> str:
    parts = [f"【{reading}】"] if reading and reading != headword else []
    def flatten_list(item):
        if isinstance(item, str): return "" if item.startswith("redirected from") else item
        if isinstance(item, dict): return flatten_structured_content(item)
        if isinstance(item, list): return " ".join(piece for piece in (flatten_list(value) for value in item) if piece)
        return ""
    if isinstance(definitions, list):
        values = []
        for definition in definitions[:6]:
            text = flatten_list(definition).strip()
            if text and text not in values:
                values.append(text)
        for index, text in enumerate(values):
            parts.append(f"\n{index + 1}. {text}" if len(values) > 1 else f"\n{text}")
    result = "\n".join(parts)
    result = re.sub(r"[ \t]+", " ", result)
    return re.sub(r"\n{3,}", "\n\n", result).replace("• • ", "• ").strip()


def find_redirect_target(definitions) -> str:
    if isinstance(definitions, dict):
        data = definitions.get("data")
        if isinstance(data, dict) and data.get("content") == "redirect-glossary":
            return flatten_structured_content(definitions).replace("⟶", "").strip()
        return find_redirect_target(definitions.get("content"))
    if isinstance(definitions, list):
        return next((target for target in (find_redirect_target(value) for value in definitions) if target), "")
    return ""


def _load_yomitan(path: str | Path) -> tuple[dict, list[tuple[str, object]]]:
    source = Path(path)
    try:
        if source.is_dir():
            meta = json.loads((source / "index.json").read_text(encoding="utf-8")) if (source / "index.json").exists() else {}
            banks = [(item.name, json.loads(item.read_text(encoding="utf-8"))) for item in sorted(source.glob("term_bank_[0-9]*.json"))]
        else:
            with zipfile.ZipFile(source, "r") as archive:
                names = archive.namelist()
                meta = json.load(archive.open("index.json")) if "index.json" in names else {}
                banks = [(item, json.load(archive.open(item))) for item in sorted(name for name in names if re.match(r"term_bank_\d+\.json$", name))]
    except (OSError, zipfile.BadZipFile, json.JSONDecodeError, UnicodeDecodeError) as error:
        raise ValueError(f"Invalid Yomitan source: {source}: {error}") from error
    if not banks:
        raise ValueError("Invalid Yomitan source: no term_bank_N.json files found")
    return meta, banks


def convert_yomitan(source_path: str, output_dir: str, name: str = "vocab") -> None:
    print(f"Loading {source_path}...")
    meta, banks = _load_yomitan(source_path)
    print(f"  Dictionary: {meta.get('title', '(unknown)')}\n  Format version: {meta.get('format', meta.get('version', '?'))}\n  Found {len(banks)} term bank files")
    all_entries, canonical = [], {}
    for _bank_name, entries in banks:
        for entry in entries:
            if not isinstance(entry, list) or len(entry) < 6 or not isinstance(entry[0], str) or not entry[0]: continue
            headword, reading, rules, score, definitions = entry[0], entry[1] if isinstance(entry[1], str) else "", entry[3] if isinstance(entry[3], str) else "", entry[4], entry[5]
            redirect = find_redirect_target(definitions)
            all_entries.append((headword, reading, score, definitions, redirect, rules))
            if not redirect:
                definition = format_definition_yomitan(headword, reading, definitions)
                if definition:
                    priority = max(0, min(255, int(score) + 128)) if isinstance(score, (int, float)) else 100
                    previous = canonical.get(headword)
                    if previous is None or priority > previous[1]: canonical[headword] = (definition, priority)
    records, entry_count, skipped_headwords = [], 0, 0
    for headword, reading, score, definitions, redirect, rules in all_entries:
        if redirect:
            target = canonical.get(redirect)
            if not target: continue
            definition, priority = f"= {redirect}\n{target[0]}", target[1]
        else:
            definition = format_definition_yomitan(headword, reading, definitions)
            if not definition: continue
            priority = max(0, min(255, int(score) + 128)) if isinstance(score, (int, float)) else 100
        flags = pos_flags_from_tags(rules.split()) if rules.strip() else POS_OTHER
        headword_bytes = headword.encode("utf-8")
        if len(headword_bytes) < HEADWORD_SIZE:
            records.append((headword_bytes, definition.encode("utf-8"), priority, flags))
        else:
            skipped_headwords += 1
        if reading and reading != headword and not redirect:
            reading_bytes = reading.encode("utf-8")
            if len(reading_bytes) < HEADWORD_SIZE:
                reading_definition = format_definition_yomitan(reading, reading, definitions)
                if reading_definition: records.append((reading_bytes, reading_definition.encode("utf-8"), priority, flags | POS_READING))
            else:
                skipped_headwords += 1
        entry_count += 1
    print(f"Processed {entry_count} Yomitan entries -> {len(records)} index records")
    write_binary(records, output_dir, name)
    if skipped_headwords:
        print(f"Skipped {skipped_headwords} headword{'s' if skipped_headwords != 1 else ''} >= {HEADWORD_SIZE} bytes")


def convert_mdict(mdx_path: str, output_dir: str, name: str = "vocab") -> None:
    try:
        from readmdict import MDX
    except ImportError as error:
        raise RuntimeError("MDict conversion requires optional dependency readmdict; install it with: pip install readmdict") from error
    records = []
    skipped_headwords = 0
    for key, value in MDX(mdx_path).items():
        headword, raw_definition = key.decode("utf-8", "replace").strip(), value.decode("utf-8", "replace").strip()
        definition = strip_html(raw_definition)
        if not headword or not definition or raw_definition.startswith("@@@LINK="):
            continue
        headword_bytes = headword.encode("utf-8")
        if len(headword_bytes) >= HEADWORD_SIZE:
            skipped_headwords += 1
            continue
        records.append((headword_bytes, definition.encode("utf-8"), 100, 0))
    write_binary(records, output_dir, name)
    if skipped_headwords:
        print(f"Skipped {skipped_headwords} headword{'s' if skipped_headwords != 1 else ''} >= {HEADWORD_SIZE} bytes")


def detect_format(path: str) -> str:
    lower = path.lower()
    return "mdict" if lower.endswith(".mdx") else "yomitan" if lower.endswith(".zip") or Path(path).is_dir() else "jmdict"


def download_jmdict(output_path: str) -> str:
    target = output_path + ".tgz"
    if not os.path.exists(target):
        print(f"Downloading {JMDICT_URL}...")
        urllib.request.urlretrieve(JMDICT_URL, target)
    return target


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert JMdict, Yomitan, or MDict inputs to CrossInk dictionary files.")
    parser.add_argument("--input")
    parser.add_argument("--output-dir", default="output")
    parser.add_argument("--name", default="vocab", choices=("vocab", "names", "grammar"))
    parser.add_argument("--format", choices=("jmdict", "yomitan", "mdict"))
    args = parser.parse_args()
    if not args.input and args.name != "vocab":
        parser.error("--name names/grammar requires --input; do not put downloaded JMdict in that slot")
    source = args.input or download_jmdict(str(Path(args.output_dir) / "jmdict-eng"))
    fmt = args.format or detect_format(source)
    print(f"Detected format: {fmt}")
    try:
        {"jmdict": convert_jmdict, "yomitan": convert_yomitan, "mdict": convert_mdict}[fmt](source, args.output_dir, args.name)
    except RuntimeError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1) from error


if __name__ == "__main__":
    main()
