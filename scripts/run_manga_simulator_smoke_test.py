#!/usr/bin/env python3
"""Run the manga reader smoke flow with an isolated, dependency-free fixture."""

from __future__ import annotations

import argparse
import os
import shutil
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

from run_simulator_smoke_test import (
    CRASH_PATTERNS,
    DEFAULT_BOOK,
    ROOT,
    build_simulator,
    prepare_fs,
    program_path,
)


MANGA_BOOK = "/books/smoke-manga"
EXPECTED_STEPS = (
    "Rendering Manga first panel from overview",
    "Rendering Manga overview restored from panel",
    "Rendering Manga chapter list opened",
    "Rendering Manga bookmark list opened",
    "Rendering Manga percent selector opened",
    "Rendering Manga manual refresh after child menu",
    "Rendering Books reopened for manga folder",
    "Rendering Manga reader reopened at persisted position",
    "Rendering Manga Recent Books grid",
    "Rendering Manga reopened from Recent Books",
    "Verified manga recents, progress, stats, and safe cache clear",
    "Verified manga sleep cover generation and corrupt-cache recovery",
)
GRAYSCALE_HASH_RE = re.compile(
    r"Grayscale hash page=(?P<page>\d+) panel=(?P<panel>-?\d+) orientation=(?P<orientation>\d+) "
    r"plane=(?P<plane>BW|LSB|MSB|RESTORED_BW) hash=(?P<hash>[0-9a-fA-F]{8})"
)


def validate_prefetch_stress(output: str) -> None:
    """Require a distinct held worker to finish cancellation before each foreground action."""
    for marker in (
        "Verified prefetch holds source before foreground intent",
        "Verified coalesced input page=0 panel=-1 with menu closed",
        "Verified coalesced input page=1 panel=0 with menu closed",
        "Rendering Manga Confirm on menu drain selected chapter",
        "Rendering Manga duplicate menu request consumed",
        "Rendering Prefetch child push drained",
        "Rendering Prefetch child pop resumed manga",
        "Rendering Prefetch manual refresh drained",
        "Rendering Prefetch replace drained",
        "Rendering Prefetch reader pop drained",
        "Verified main sleep defers before persistence while prefetch drains",
        "Verified main sleep preparation completed after worker quiescence",
        "Rendering Prefetch main sleep drained",
    ):
        if marker not in output:
            raise RuntimeError(f"prefetch stress missed lifecycle assertion: {marker}")

    cursor = 0
    for phase, activity in (
        ("child push", "ReaderOptions"),
        ("manual refresh", None),
        ("replace", "ReaderOptions"),
        ("reader pop", "Home"),
        ("main sleep", "Sleep"),
    ):
        rendered = output.find(f"Rendering Prefetch {phase} drained", cursor)
        held = output.rfind("Verified prefetch holds source before foreground intent", cursor, rendered)
        # Result::Cancelled is published only after decoder/file owners return and the cache closes.
        cancelled = output.find("Prefetch result=4 ", held, rendered) if held >= 0 else -1
        entered = output.find(f"Entering activity: {activity}", held, rendered) if activity else rendered
        if not cursor <= held < cancelled < entered <= rendered:
            raise RuntimeError(f"prefetch stress missed ordered worker drain before {phase}")
        if phase in ("replace", "reader pop", "main sleep"):
            exited = output.find("Exiting activity: MangaReader", held, entered)
            if not cancelled < exited < entered:
                raise RuntimeError(f"prefetch stress exited manga before worker drain during {phase}")
        cursor = rendered + len(f"Rendering Prefetch {phase} drained")


def validate_grayscale_hashes(output: str) -> None:
    """Require complete, repeatable page and panel grayscale framebuffer sequences."""
    sequences: dict[tuple[int, int, int], list[tuple[int, int, int, int]]] = {}
    current: dict[tuple[int, int, int], list[int]] = {}
    plane_index = {"BW": 0, "LSB": 1, "MSB": 2, "RESTORED_BW": 3}

    for match in GRAYSCALE_HASH_RE.finditer(output):
        key = (int(match["page"]), int(match["panel"]), int(match["orientation"]))
        plane = match["plane"]
        value = int(match["hash"], 16)
        if plane == "BW":
            if key in current:
                raise RuntimeError(f"incomplete grayscale hash sequence before repeated BW for {key}")
            current[key] = [value]
            continue
        values = current.get(key)
        if values is None or len(values) != plane_index[plane]:
            raise RuntimeError(f"out-of-order grayscale hash plane {plane} for {key}")
        values.append(value)
        if plane == "RESTORED_BW":
            if values[3] != values[0]:
                raise RuntimeError(f"restored BW framebuffer differs from initial BW for {key}")
            sequences.setdefault(key, []).append(tuple(values))
            del current[key]

    if current:
        raise RuntimeError(f"incomplete grayscale hash sequences: {sorted(current)}")

    repeated_pages = [runs for (_, panel, _), runs in sequences.items() if panel < 0 and len(runs) >= 2]
    repeated_panels = [runs for (_, panel, _), runs in sequences.items() if panel >= 0 and len(runs) >= 2]
    if not repeated_pages or not repeated_panels:
        raise RuntimeError("grayscale smoke did not repeat both a page and panel in the same orientation")
    for runs in (*repeated_pages, *repeated_panels):
        if any(run != runs[0] for run in runs[1:]):
            raise RuntimeError("cold/warm grayscale framebuffer hashes differ")
    if "Pixel cache created:" not in output or "Pixel cache hit:" not in output:
        raise RuntimeError("grayscale smoke did not exercise both cold and warm pixel-cache paths")
    if "Pixel cache source geometry hit:" not in output:
        raise RuntimeError("grayscale smoke did not reuse source dimensions from pixel-cache metadata")


def screenshot_frame_hash(path: Path) -> int:
    """Undo ScreenshotUtil's 90-degree packing and hash the original framebuffer."""
    raw = path.read_bytes()
    offset = struct.unpack_from("<I", raw, 10)[0]
    out_width, out_height = struct.unpack_from("<ii", raw, 18)
    if raw[:2] != b"BM" or out_width <= 0 or out_height <= 0 or struct.unpack_from("<H", raw, 28)[0] != 1:
        raise RuntimeError("invalid manga screenshot bitmap")
    row_bytes = ((out_width + 31) // 32) * 4
    if len(raw) != offset + row_bytes * out_height:
        raise RuntimeError("truncated manga screenshot")
    width, height = out_height, out_width
    frame = bytearray(width * height // 8)
    for out_y in range(out_height):
        for out_x in range(out_width):
            pixel = (raw[offset + out_y * row_bytes + out_x // 8] >> (7 - out_x % 8)) & 1
            src_x, src_y = width - 1 - out_y, height - 1 - out_x
            frame[src_y * (width // 8) + src_x // 8] |= pixel << (7 - src_x % 8)
    value = 2166136261
    for byte in frame:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def one_bit_bmp(width: int = 8, height: int = 8) -> bytes:
    """Return a small valid Windows 1-bit BMP using only the standard library."""
    row_bytes = ((width + 31) // 32) * 4
    pixels = bytearray()
    for row in range(height):
        pattern = 0xAA if row % 2 == 0 else 0x55
        pixels.extend(bytes([pattern]) + bytes(row_bytes - 1))
    pixel_offset = 14 + 40 + 8
    file_size = pixel_offset + len(pixels)
    file_header = struct.pack("<2sIHHI", b"BM", file_size, 0, 0, pixel_offset)
    info_header = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 1, 0, len(pixels), 2835, 2835, 2, 2)
    palette = b"\x00\x00\x00\x00\xff\xff\xff\x00"
    return file_header + info_header + palette + pixels


def grayscale_bmp(width: int = 12, height: int = 8) -> bytes:
    """Four native gray levels in an asymmetric real 8-bit BMP."""
    row_bytes = (width + 3) // 4 * 4
    pixels = b"".join(bytes((x + y) % 4 for x in range(width)) + bytes(row_bytes - width) for y in range(height))
    offset = 14 + 40 + 16
    header = struct.pack("<2sIHHI", b"BM", offset + len(pixels), 0, 0, offset)
    info = struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 8, 0, len(pixels), 2835, 2835, 4, 4)
    palette = b"".join(bytes([value, value, value, 0]) for value in (0, 85, 170, 255))
    return header + info + palette + pixels


def prepare_manga_fixture(fs_root: Path) -> Path:
    """Build two original-writer-format pages from the checked-in golden record."""
    fixture_dir = ROOT / "test" / "manga_format" / "fixtures"
    source_index = (fixture_dir / "panels.idx").read_bytes()
    source_data = (fixture_dir / "panels.dat").read_bytes()
    first_offset, first_length, width, height = struct.unpack_from("<IIHH", source_index, 8)
    page = source_data[first_offset : first_offset + first_length]
    if len(page) != first_length:
        raise RuntimeError("checked-in manga page fixture is truncated")

    if os.environ.get("CROSSINK_SIMULATOR_MANGA_OCR"):
        text = b"Reader text"
        if os.environ.get("CROSSINK_SIMULATOR_MANGA_QR_REVIEW"):
            mixed = "aé漢😀".encode()
            text = mixed * (2953 // len(mixed)) + b"a" * (2953 % len(mixed))
        translation = ("Stored translation without network. " * 160).encode()
        page = struct.pack("<BB", 1, 0)
        page += struct.pack("<HHHHBBH", 0, 0, width, height, 1, 0, len(translation)) + translation
        page += struct.pack("<HHHHH", 10, 10, 100, 80, len(text)) + text
    manga_dir = fs_root / MANGA_BOOK.removeprefix("/")
    panels_dir = manga_dir / "panels"
    panels_dir.mkdir(parents=True)
    index = struct.pack("<II", 2, 2)
    index += struct.pack("<IIHH", 0, len(page), width, height)
    second_page = page
    if os.environ.get("CROSSINK_SIMULATOR_MANGA_OCR"):
        second_page = struct.pack("<BBHHHHBBH", 1, 0, 0, 0, width, height, 0, 0, len(translation)) + translation
    index += struct.pack("<IIHH", len(page), len(second_page), width, height)
    (manga_dir / "panels.idx").write_bytes(index)
    (manga_dir / "panels.dat").write_bytes(page + second_page)
    (manga_dir / "meta.bin").write_bytes((fixture_dir / "meta-language.bin").read_bytes())
    if os.environ.get("CROSSINK_SIMULATOR_MANGA_OCR"):
        values = ("Manga OCR smoke", "Fixture author", "en")
        (manga_dir / "meta.bin").write_bytes(struct.pack("<IHH", 1, len(values[0]), len(values[1])) + values[0].encode() + values[1].encode() + struct.pack("<H", len(values[2])) + values[2].encode())
    toc = struct.pack("<II", 1, 2)
    for page_index, label in ((0, "Cover"), (1, "Second page")):
        encoded = label.encode("utf-8")
        toc += struct.pack("<IH", page_index, len(encoded)) + encoded
    (manga_dir / "toc.idx").write_bytes(toc)

    bmp = grayscale_bmp() if os.environ.get("CROSSINK_SIMULATOR_MANGA_GRAYSCALE") else one_bit_bmp()
    for page_index in range(2):
        (manga_dir / f"page_{page_index:04d}.bmp").write_bytes(bmp)
        (panels_dir / f"p{page_index}_0.bmp").write_bytes(bmp)
    if os.environ.get("CROSSINK_SIMULATOR_MANGA_OCR"):
        for suffix, empty in (("-empty", True), ("-crop-only", False)):
            target = fs_root / "manga-ocr-fixtures" / suffix.removeprefix("-")
            (target / "panels").mkdir(parents=True)
            record = struct.pack("<BBHHHHBBH", 1, 0, 0, 0, width, height, 0, 0, 0) if empty else page
            (target / "panels.dat").write_bytes(record)
            (target / "panels.idx").write_bytes(struct.pack("<IIIIHH", 2, 1, 0, len(record), width, height))
            (target / "meta.bin").write_bytes((manga_dir / "meta.bin").read_bytes())
            (target / "panels" / "p0_0.bmp").write_bytes(bmp)
            if empty:
                (target / "page_0000.bmp").write_bytes(bmp)
    return manga_dir


def prepare_manga_lookup_dictionary(fs_root: Path) -> None:
    """Long local definitions exercise paging and nested lookup without downloads."""
    records = {"Reader": "text " * 400, "text": "Reader"}
    index, body = bytearray(), bytearray()
    for word, definition in sorted(records.items(), key=lambda item: item[0].casefold()):
        encoded = definition.encode()
        index += word.encode() + b"\0" + struct.pack(">II", len(body), len(encoded))
        body += encoded
    for language, folder in (("en", "smoke"), ("fr", "book")):
        base = fs_root / "dictionaries" / language / folder / "dict-data"
        base.with_suffix(".idx").write_bytes(index)
        base.with_suffix(".dict").write_bytes(body)
        base.with_suffix(".ifo").write_text(
            "StarDict's dict ifo file\nversion=3.0.0\n"
            f"bookname=Manga {language} fixture\nwordcount={len(records)}\n"
            f"idxfilesize={len(index)}\nsametypesequence=m\n"
        )


def validate_progress(fs_root: Path) -> None:
    crc = zlib.crc32(MANGA_BOOK.encode("utf-8")) & 0xFFFFFFFF
    state_path = fs_root / ".crosspoint" / "manga-state" / f"{crc}.bin"
    record = state_path.read_bytes()
    if len(record) != 12:
        raise RuntimeError(f"manga progress record has {len(record)} bytes, expected 12")
    magic, version, flags, panel, page = struct.unpack("<4sBBhI", record)
    expected = (b"MGPR", 1, 0x01, 0, 1)
    if (magic, version, flags, panel, page) != expected:
        raise RuntimeError(
            "unexpected manga progress: "
            f"magic={magic!r} version={version} flags=0x{flags:02x} panel={panel} page={page}; "
            "expected page 1 panel 0 with panels-only enabled and rotation disabled"
        )


def validate_library(fs_root: Path, x4_touch: bool = False) -> None:
    crc = zlib.crc32(MANGA_BOOK.encode("utf-8")) & 0xFFFFFFFF
    cache = fs_root / ".crosspoint" / f"manga_{crc}"
    cover = cache / "thumb_v3_123x180.bmp"
    data = cover.read_bytes()
    width, height = struct.unpack_from("<ii", data, 18)
    if data[:2] != b"BM" or (width, abs(height)) != (123, 180) or len(data) != 62 + 16 * 180:
        raise RuntimeError("manga Recent Books thumbnail is missing or malformed")
    identity = cover.with_suffix(cover.suffix + ".src").read_bytes()
    if len(identity) != 40 or identity[:4] != b"MCG3":
        raise RuntimeError("manga cover identity is missing or has the wrong cache version")
    if struct.unpack_from("<4I", identity, 12) != (123, 180, width, abs(height)):
        raise RuntimeError("manga cover identity dimensions do not match the request and BMP")
    if zlib.crc32(data) & 0xFFFFFFFF != struct.unpack_from("<I", identity, 28)[0]:
        raise RuntimeError("manga cover identity checksum does not match the BMP")
    if x4_touch:
        # X4 Home requests a responsive portrait cover. Recent Books retains its
        # separately verified 123x180 thumbnail; require an actual Home cache
        # with dimensions and source identity matching its encoded filename.
        home_covers = [path for path in cache.glob("thumb_v3_*x*.bmp") if path != cover]
        valid_home = False
        for home in home_covers:
            match = re.fullmatch(r"thumb_v3_(\d+)x(\d+)\.bmp", home.name)
            if not match:
                continue
            requested_width, requested_height = map(int, match.groups())
            if not 0 < requested_width < requested_height <= 800:
                continue
            bitmap = home.read_bytes()
            identity = home.with_suffix(home.suffix + ".src").read_bytes()
            actual_width, actual_height = struct.unpack_from("<ii", bitmap, 18)
            row_bytes = ((actual_width + 31) // 32) * 4
            if (bitmap[:2] != b"BM" or (actual_width, abs(actual_height)) != (requested_width, requested_height)
                    or len(bitmap) != 62 + row_bytes * requested_height
                    or len(identity) != 40 or identity[:4] != b"MCG3"
                    or struct.unpack_from("<4I", identity, 12) !=
                       (requested_width, requested_height, actual_width, abs(actual_height))
                    or zlib.crc32(bitmap) & 0xFFFFFFFF != struct.unpack_from("<I", identity, 28)[0]):
                raise RuntimeError("X4 manga Home cover or identity is malformed")
            valid_home = True
        if not valid_home:
            raise RuntimeError("X4 manga Home cover was not generated")
    elif os.environ.get("CROSSINK_SIMULATOR_SMOKE_THEME", "1") == "1" and not (cache / "thumb_v3_151x226.bmp").is_file():
        raise RuntimeError("manga Home cover was not generated")
    if os.environ.get("CROSSINK_SIMULATOR_MANGA_GRAYSCALE"):
        pixel_files = list(cache.glob("pixels_v1_*.pxc"))
        if not pixel_files:
            raise RuntimeError("grayscale manga did not publish any pixel caches")
        for pixel_path in pixel_files:
            raw = pixel_path.read_bytes()
            width, height = struct.unpack_from("<HH", raw)
            envelope = pixel_path.with_suffix(pixel_path.suffix + ".id").read_bytes()
            if len(raw) != 4 + ((width + 3) // 4) * height or len(envelope) != 48 or envelope[:4] != b"MPX1":
                raise RuntimeError("invalid manga pixel-cache publication")
            if zlib.crc32(raw[4:]) & 0xFFFFFFFF != struct.unpack_from("<I", envelope, 44)[0]:
                raise RuntimeError("manga pixel-cache checksum mismatch")
    if not (cache / "stats_v6.bin").is_file():
        raise RuntimeError("manga reading statistics were not saved")


def run_smoke(args: argparse.Namespace) -> int:
    if args.build:
        build_simulator(args.env)
    program = program_path(args.env)
    if not program.exists():
        print(f"Simulator binary not found: {program}", file=sys.stderr)
        print(f"Run: pio run -e {args.env}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="crossink-manga-sim-smoke-") as temp_name:
        temp_root = Path(temp_name)
        prepare_fs(temp_root, DEFAULT_BOOK)
        prepare_manga_fixture(temp_root / "fs_")
        if os.environ.get("CROSSINK_SIMULATOR_MANGA_OCR"):
            prepare_manga_lookup_dictionary(temp_root / "fs_")

        env = os.environ.copy()
        env["CROSSINK_SIMULATOR_SMOKE_TEST"] = "1"
        env["CROSSINK_SIMULATOR_SMOKE_BOOK"] = MANGA_BOOK
        if args.headless:
            env.setdefault("SDL_VIDEODRIVER", "dummy")

        print(f"Running manga smoke test with isolated fs_: {temp_root / 'fs_'}", flush=True)
        try:
            proc = subprocess.run(
                [str(program)],
                cwd=temp_root,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=args.timeout,
            )
        except subprocess.TimeoutExpired as error:
            output = error.stdout or ""
            if isinstance(output, bytes):
                output = output.decode("utf-8", errors="replace")
            print(output, end="")
            print(f"Manga simulator smoke test timed out after {args.timeout}s", file=sys.stderr)
            return 2
        output = proc.stdout
        try:
            if proc.returncode != 0:
                raise RuntimeError(f"simulator exited with status {proc.returncode}")
            checked_output = output
            failure_case = os.environ.get("CROSSINK_SIMULATOR_MANGA_RENDER_FAILURE")
            if failure_case == "restore":
                checked_output = "\n".join(line for line in output.splitlines()
                                            if "[ERR] [MANGA] Could not restore BW image after grayscale" not in line)
            if os.environ.get("CROSSINK_SIMULATOR_MANGA_FAILURES"):
                checked_output = "\n".join(line for line in checked_output.splitlines()
                    if "[ERR] [MANGA] Cache retained because durable state could not be saved" not in line)
            if os.environ.get("CROSSINK_SIMULATOR_MANGA_QR_REVIEW"):
                checked_output = "\n".join(line for line in checked_output.splitlines()
                    if "[ERR] [MANGA] Cannot allocate QR activity" not in line)
            for pattern in (*CRASH_PATTERNS, "[ERR] [MANGA]", "[ERR] [MNG]", "[ERR] [MGPR]", "[ERR] [BKS]"):
                if pattern in checked_output:
                    raise RuntimeError(f"simulator output contained crash pattern: {pattern}")
            if "Simulator smoke test passed" not in output:
                raise RuntimeError("simulator did not print its success marker")
            if os.environ.get("CROSSINK_SIMULATOR_MANGA_TOUCH_LOOKUP_ONLY"):
                capture_dir = os.environ.get("CROSSINK_SIMULATOR_MANGA_TOUCH_CAPTURE_DIR")
                if capture_dir:
                    Path(capture_dir).mkdir(parents=True, exist_ok=True)
                    for name in ("touch-dictionary.bmp",):
                        shutil.copyfile(temp_root / "fs_" / name, Path(capture_dir) / name)
                for marker in ("Verified manga bubble tap opens dictionary directly",
                               "Verified manga dictionary next/previous terms in tapped bubble",
                               "Rendering Manga touch lookup returns to held panel"):
                    if marker not in output:
                        raise RuntimeError(f"missing touch lookup contract: {marker}")
                print(output, end="")
                return 0
            if os.environ.get("CROSSINK_SIMULATOR_MANGA_ACTIVE_EVENTS"):
                count = 5 if args.env == "simulator" else 6
                for mode in range(count):
                    if f"Verified active manga automatic event cancellation mode={mode}" not in output:
                        raise RuntimeError("missing active automatic cancellation event")
                print(output, end="")
                return 0
            if os.environ.get("CROSSINK_SIMULATOR_MANGA_TOUCH_PAGING"):
                if "Verified touch-only manga paging in both directions and fresh popup hitboxes" not in output:
                    raise RuntimeError("missing touch paging/hitbox evidence")
                print(output, end="")
                return 0
            if os.environ.get("CROSSINK_SIMULATOR_MANGA_QR_REVIEW"):
                for mode in range(3):
                    if f"Verified maximum mixed UTF-8 QR child redraw/allocation mode={mode}" not in output:
                        raise RuntimeError("missing actual QR child ownership/allocation evidence")
                print(output, end="")
                return 0
            if os.environ.get("CROSSINK_SIMULATOR_MANGA_REVIEW"):
                marker = {
                    "deferred": "Verified deferred-render menu interleaving rearms one automatic deadline",
                    "release": "Verified one-shot page release survives completion render service exactly once",
                    "active_cancel": "Verified active cancellation precedes completion render service",
                }.get(os.environ["CROSSINK_SIMULATOR_MANGA_REVIEW"],
                      "Verified one-shot lookup survives warming and rejects owned drain")
                if marker not in output:
                    raise RuntimeError(f"missing interleaving contract: {marker}")
                print(output, end="")
                return 0
            if os.environ.get("CROSSINK_SIMULATOR_MANGA_FAILURES"):
                for marker in ("Verified cache stats failure retains frozen target and usable reader retry=0",
                               "Verified cache stats failure retains frozen target and usable reader retry=1",
                               "Verified menu cache retry preserves dictionary/history/stats and accepted tail",
                               "Verified menu cache regeneration and idempotent exit",
                               "Verified deferred manga screenshot write failure feedback"):
                    if marker not in output:
                        raise RuntimeError(f"missing failure contract: {marker}")
                print(output, end="")
                return 0
            if failure_case:
                marker = ("Verified failed BW restore prevents manga screenshot" if failure_case == "restore"
                          else "Verified cache deletion feedback without closed-book rendering")
                if marker not in output:
                    raise RuntimeError("missing actual render failure evidence")
                if failure_case == "restore" and list((temp_root / "fs_" / "screenshots").rglob("*.bmp")):
                    raise RuntimeError("failed BW restore saved an error-page screenshot")
                print(output, end="")
                return 0
            if os.environ.get("CROSSINK_SIMULATOR_MANGA_MENU"):
                for scope in (-1, 0):
                    for row in range(16):
                        marker = f"Verified manga menu action {row} scope {scope}"
                        if marker not in output:
                            raise RuntimeError(f"missing menu evidence: {marker}")
                if "Verified actual manga status BW/LSB/MSB/restored cleanup pixels in four orientations" not in output:
                    raise RuntimeError("missing actual renderer status plane evidence")
                if len(list((temp_root / "fs_" / "screenshots").rglob("*.bmp"))) < 2:
                    raise RuntimeError("reader menu screenshots were not written")
                expected_hashes = {int(value, 16) for value in re.findall(r"Verified manga screenshot scope=-?\d+ framebuffer=([0-9a-fA-F]+)", output)}
                screenshots = list((temp_root / "fs_" / "screenshots").rglob("*.bmp"))
                if not expected_hashes or {screenshot_frame_hash(path) for path in screenshots} != expected_hashes:
                    raise RuntimeError("saved manga screenshot pixels differ from labeled reader frame")
                if any("Manga-OCR-smoke_p1_0pct_" not in path.name for path in screenshots):
                    raise RuntimeError("manga screenshot filename has wrong page/title/progress")
                for marker in ("Verified both manga shortcuts reject modal, lock and suspension",
                               "Verified applicable manga settings change persists on child return"):
                    if marker not in output:
                        raise RuntimeError(f"missing menu contract: {marker}")
                print(output, end="")
                print("Verified all sixteen manga rows from overview/panel, shared settings, shortcuts and status planes.")
                return 0
            if os.environ.get("CROSSINK_SIMULATOR_MANGA_OCR"):
                for marker in ("Verified manga scan cache loaded=0 cursor=0",
                               "Verified manga scan cache loaded=1 cursor=1",
                               "Verified dictionary scan cache loaded:",
                               "Verified manga dictionary ready cursor=0", "Verified manga dictionary ready cursor=1",
                               "Rendering Manga clipping feedback", "Rendering Manga stored panel translation",
                               "Rendering Manga translation next page", "Rendering Manga lookup history",
                               "Rendering Manga overview shared lookup", "Rendering Manga stored overview translation",
                               "Verified manga saved lookup history", "Verified manga reader font restored",
                               "Verified manga image framebuffer restored", "No OCR text page=1 panel=0", "No OCR text page=0 panel=0",
                               "Verified manga empty OCR feedback", "Verified manga empty translation state",
                               "Verified manga unavailable dictionary state", "Rendering Manga translation without OCR",
                               "Rendering Manga translation without dictionary", "Rendering Manga text fallback shared lookup",
                               "Rendering Manga lookup after repeated pending Confirm", "Forced lookup teardown from ActivityManager lifecycle",
                               "Rendering Manga reopened after forced lookup exit") :
                    if marker not in output:
                        raise RuntimeError(f"missing OCR smoke evidence: {marker}")
                clippings = temp_root / "fs_" / "My Clippings.txt"
                if not clippings.exists():
                    raise RuntimeError("manga lookup did not save its clipping")
                entries = [entry for entry in clippings.read_text().split("\n==========\n") if entry]
                if len(entries) != 1 or "\n\n" not in entries[0]:
                    raise RuntimeError("unexpected manga clipping record structure")
                header, selected_text = entries[0].split("\n\n", 1)
                if selected_text != "Reader":
                    raise RuntimeError(f"manga clipping text differs from exact source: {selected_text!r}")
                lines = header.splitlines()
                if len(lines) != 2 or not re.fullmatch(
                    r"- Your Highlight on Page 1 \| Page 1, panel 1(?: \| Added on .*)?", lines[1]
                ):
                    raise RuntimeError("manga clipping lost physical page/panel context")
            for step in EXPECTED_STEPS:
                if step not in output:
                    raise RuntimeError(f"simulator skipped expected manga lifecycle step: {step}")
            if os.environ.get("CROSSINK_SIMULATOR_MANGA_PREFETCH_STRESS"):
                validate_prefetch_stress(output)
            if os.environ.get("CROSSINK_SIMULATOR_MANGA_GRAYSCALE"):
                validate_grayscale_hashes(output)
            validate_progress(temp_root / "fs_")
            validate_library(temp_root / "fs_", args.env == "x4-pro-simulator")
        except (OSError, RuntimeError) as error:
            print(output, end="")
            print(f"Manga simulator smoke test failed: {error}", file=sys.stderr)
            return 2

    print(output, end="")
    print("Verified persisted manga page, panel, panels-only mode, and rotation setting.")
    if os.environ.get("CROSSINK_SIMULATOR_MANGA_GRAYSCALE"):
        print("Verified cold/warm manga BW, grayscale-plane, and restored-BW framebuffer hashes.")
    else:
        print("This exercises the monochrome BMP renderer; JPEG/PNG decoders remain simulator stubs.")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--env",
        choices=("simulator", "sticky-simulator", "x4-pro-simulator"),
        default="simulator",
        help="PlatformIO simulator environment to build and run",
    )
    parser.add_argument("--timeout", type=int, default=45, help="Seconds before the simulator run is treated as hung")
    parser.add_argument("--no-build", dest="build", action="store_false", help="Run the existing simulator binary")
    parser.add_argument(
        "--window", dest="headless", action="store_false", help="Show the SDL window instead of using dummy video"
    )
    parser.set_defaults(build=True, headless=True)
    return parser.parse_args()


def main() -> int:
    return run_smoke(parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
