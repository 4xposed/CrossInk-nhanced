#!/usr/bin/env python3
"""Generate and validate Matcha version-1 dictionary sparse-index sidecars."""

from __future__ import annotations

import os
import struct
import sys
from pathlib import Path


RECORD_SIZE = 40
HEADWORD_SIZE = 32
SPX_MAGIC = b"CPSPX1\0\0"
SPX_VERSION = 1
SPX_HEADER_SIZE = 32
SPX_STRIDE = 48
DICTS = ("vocab", "names", "grammar", "jmdict", "jmnedict")
USAGE = "usage: gen_dict_spx.py /path/to/sdcard/dictionaries/jp"


def build_spx_bytes(idx_bytes: bytes) -> bytes:
    """Return the Matcha v1 sidecar for a validated flat index."""
    if len(idx_bytes) % RECORD_SIZE:
        raise ValueError(f"idx size {len(idx_bytes)} is not a multiple of {RECORD_SIZE}")
    count = len(idx_bytes) // RECORD_SIZE
    fine_count = (count + SPX_STRIDE - 1) // SPX_STRIDE
    header = SPX_MAGIC + struct.pack("<IIIII", SPX_VERSION, SPX_STRIDE, count, fine_count, 0)
    header += b"\0" * (SPX_HEADER_SIZE - len(header))
    checkpoints = b"".join(
        idx_bytes[record * RECORD_SIZE : record * RECORD_SIZE + HEADWORD_SIZE]
        for record in range(0, count, SPX_STRIDE)
    )
    return header + checkpoints


def validate_spx(idx_bytes: bytes, spx_bytes: bytes) -> tuple[int, int]:
    """Reject stale or malformed sidecars before they are published."""
    if len(idx_bytes) % RECORD_SIZE:
        raise ValueError(f"idx size {len(idx_bytes)} is not a multiple of {RECORD_SIZE}")
    if len(spx_bytes) < SPX_HEADER_SIZE:
        raise ValueError("spx is shorter than its header")
    if spx_bytes[:8] != SPX_MAGIC:
        raise ValueError("spx magic is invalid")
    version, stride, count, fine_count, reserved = struct.unpack_from("<IIIII", spx_bytes, 8)
    expected_count = len(idx_bytes) // RECORD_SIZE
    expected_fine_count = (expected_count + SPX_STRIDE - 1) // SPX_STRIDE
    if (version, stride, count, fine_count, reserved) != (
        SPX_VERSION,
        SPX_STRIDE,
        expected_count,
        expected_fine_count,
        0,
    ):
        raise ValueError("spx header does not match idx")
    if len(spx_bytes) != SPX_HEADER_SIZE + fine_count * HEADWORD_SIZE:
        raise ValueError("spx checkpoint section has an invalid size")
    for checkpoint, record in enumerate(range(0, count, stride)):
        expected = idx_bytes[record * RECORD_SIZE : record * RECORD_SIZE + HEADWORD_SIZE]
        actual_offset = SPX_HEADER_SIZE + checkpoint * HEADWORD_SIZE
        if spx_bytes[actual_offset : actual_offset + HEADWORD_SIZE] != expected:
            raise ValueError(f"spx checkpoint {checkpoint} does not match idx")
    return count, fine_count


def gen_one(idx_path: str | Path, spx_path: str | Path) -> tuple[int, int]:
    """Atomically generate one validated sidecar from an existing index."""
    idx_path = Path(idx_path)
    spx_path = Path(spx_path)
    idx_bytes = idx_path.read_bytes()
    spx_bytes = build_spx_bytes(idx_bytes)
    validate_spx(idx_bytes, spx_bytes)
    temp_path = spx_path.with_name(spx_path.name + ".tmp")
    if temp_path.exists():
        raise FileExistsError(f"stale temporary output exists: {temp_path}")
    try:
        with temp_path.open("xb") as output:
            output.write(spx_bytes)
        validate_spx(idx_bytes, temp_path.read_bytes())
        os.replace(temp_path, spx_path)
    except Exception:
        if temp_path.exists():
            temp_path.unlink()
        raise
    return validate_spx(idx_bytes, spx_bytes)


def main() -> None:
    if len(sys.argv) == 2 and sys.argv[1] in ("-h", "--help"):
        print(USAGE)
        return
    if len(sys.argv) != 2:
        raise SystemExit(USAGE)
    dictionary_dir = Path(sys.argv[1])
    for name in DICTS:
        idx_path = dictionary_dir / f"{name}.idx"
        if not idx_path.exists():
            print(f"skip {name}: no {idx_path}")
            continue
        count, fine_count = gen_one(idx_path, dictionary_dir / f"{name}.spx")
        spx_size = (dictionary_dir / f"{name}.spx").stat().st_size
        print(f"{name}: {count} records -> {fine_count} checkpoints ({spx_size} bytes, stride={SPX_STRIDE})")


if __name__ == "__main__":
    main()
