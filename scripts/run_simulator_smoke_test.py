#!/usr/bin/env python3
"""Build and run the simulator smoke test against an isolated fs_ directory."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from dictionary_tools import _build_oft


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BOOK = ROOT / "test" / "epubs" / "test_reader_rendering_matrix.epub"
CRASH_PATTERNS = (
    "std::bad_alloc",
    "terminating due to uncaught exception",
    "Assertion failed",
    "Segmentation fault",
    "AddressSanitizer",
    "UndefinedBehaviorSanitizer",
)
THEMES = {
    "classic": 0,
    "lyra": 1,
    "lyra-extended": 2,
    "lyra_extended": 2,
    "lyra3": 2,
    "lyra-3-covers": 2,
    "roundedraff": 3,
    "rounded-raff": 3,
    "lyra-carousel": 4,
    "lyra_carousel": 4,
    "carousel": 4,
    "minimal": 5,
    "dashboard": 6,
}
OPDS_SIMULATOR_BOOK = "Ursula K. Le Guin - The Left Hand of Darkness.epub"


def program_path(env_name: str) -> Path:
    return ROOT / ".pio" / "build" / env_name / "program"


def build_simulator(env_name: str) -> None:
    print(f"Building {env_name} simulator...", flush=True)
    proc = subprocess.run(["pio", "run", "-e", env_name], cwd=ROOT)
    if proc.returncode != 0:
        raise SystemExit(proc.returncode)


def prepare_fs(temp_root: Path, book: Path) -> str:
    books_dir = temp_root / "fs_" / "books"
    books_dir.mkdir(parents=True, exist_ok=True)

    target = books_dir / book.name
    shutil.copy2(book, target)
    # OPDS opens this as EPUB even when the active-reader fixture is an Anki deck.
    shutil.copy2(book if book.suffix.lower() == ".epub" else DEFAULT_BOOK,
                 temp_root / "fs_" / OPDS_SIMULATOR_BOOK)
    category_dir = temp_root / "fs_" / "epubs" / "nested"
    category_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(book, category_dir / book.name)
    articles = temp_root / "fs_" / "articles"
    articles.mkdir(exist_ok=True)
    (articles / "example.txt").write_text("Library smoke article.")
    prepare_dictionary(temp_root)
    return f"/books/{book.name}"


def prepare_dictionary(temp_root: Path) -> None:
    """Install a minimal language-routed StarDict for reader lookup smoke paths."""
    records = {
        "Alignment": "the arrangement of text",
        "Reader": "a person or application that reads",
        "This": "the present thing",
        "more": "a greater amount",
        "paragraph": "a section of written text",
        "text": "written words",
        "the": "definite article",
    }
    dictionary_dir = temp_root / "fs_" / "dictionaries" / "en" / "smoke"
    dictionary_dir.mkdir(parents=True, exist_ok=True)
    index = bytearray()
    definitions = bytearray()
    for word, definition in sorted(records.items(), key=lambda item: item[0].casefold()):
        encoded_word = word.encode("utf-8")
        encoded_definition = definition.encode("utf-8")
        index.extend(encoded_word)
        index.append(0)
        index.extend(struct.pack(">II", len(definitions), len(encoded_definition)))
        definitions.extend(encoded_definition)

    base = dictionary_dir / "dict-data"
    (base.with_suffix(".idx")).write_bytes(index)
    (base.with_suffix(".idx.oft")).write_bytes(_build_oft(bytes(index), 8))
    (base.with_suffix(".dict")).write_bytes(definitions)
    (base.with_suffix(".ifo")).write_text(
        "StarDict's dict ifo file\n"
        "version=3.0.0\n"
        "bookname=Simulator Smoke\n"
        f"wordcount={len(records)}\n"
        f"idxfilesize={len(index)}\n"
        "sametypesequence=m\n",
        encoding="utf-8",
    )

    fallback_dir = temp_root / "fs_" / "dictionaries" / "fr" / "book"
    fallback_dir.mkdir(parents=True, exist_ok=True)
    fallback_base = fallback_dir / "dict-data"
    (fallback_base.with_suffix(".idx")).write_bytes(index)
    (fallback_base.with_suffix(".idx.oft")).write_bytes(_build_oft(bytes(index), 8))
    (fallback_base.with_suffix(".dict")).write_bytes(definitions)
    (fallback_base.with_suffix(".ifo")).write_text(
        "StarDict's dict ifo file\n"
        "version=3.0.0\n"
        "bookname=Simulator Book Fallback\n"
        f"wordcount={len(records)}\n"
        f"idxfilesize={len(index)}\n"
        "sametypesequence=m\n",
        encoding="utf-8",
    )

    global_config_dir = temp_root / "fs_" / ".crosspoint"
    global_config_dir.mkdir(parents=True, exist_ok=True)
    (global_config_dir / "dictionary.bin").write_text("/dictionaries/en/smoke/dict-data", encoding="utf-8")
    book_cache_dir = temp_root / "fs_" / "smoke-book-cache"
    book_cache_dir.mkdir(parents=True, exist_ok=True)
    (book_cache_dir / "dictionary.bin").write_text("/dictionaries/fr/book/dict-data", encoding="utf-8")

    japanese_dir = temp_root / "fs_" / "dictionaries" / "jp"
    japanese_dir.mkdir(parents=True, exist_ok=True)
    golden_dir = ROOT / "test" / "japanese_dict_converter" / "golden" / "mini_jmdict"
    shutil.copy2(golden_dir / "vocab.idx", japanese_dir / "vocab.idx")
    shutil.copy2(golden_dir / "vocab.dat", japanese_dir / "vocab.dat")
    shutil.copy2(golden_dir / "vocab.spx", japanese_dir / "vocab.spx")


def run_smoke(args: argparse.Namespace) -> int:
    book = Path(args.book).resolve()
    if not book.exists():
        print(f"Smoke test book not found: {book}", file=sys.stderr)
        return 2

    if args.build:
        build_simulator(args.env)

    program = program_path(args.env)
    if not program.exists():
        print(f"Simulator binary not found: {program}", file=sys.stderr)
        print(f"Run: pio run -e {args.env}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="crossink-sim-smoke-") as temp_dir_name:
        temp_root = Path(temp_dir_name)
        simulator_book_path = prepare_fs(temp_root, book)

        env = os.environ.copy()
        env["CROSSINK_SIMULATOR_SMOKE_TEST"] = "1"
        env["CROSSINK_SIMULATOR_SMOKE_BOOK"] = simulator_book_path
        env["CROSSINK_SIMULATOR_SMOKE_PAGE_TURNS"] = str(args.page_turns)
        if args.lookup_only:
            env["CROSSINK_SIMULATOR_LOOKUP_REGRESSION"] = "1"
        if args.theme:
            env["CROSSINK_SIMULATOR_SMOKE_THEME"] = str(THEMES[args.theme])
        if args.headless:
            env.setdefault("SDL_VIDEODRIVER", "dummy")

        print(f"Running simulator smoke test with isolated fs_: {temp_root / 'fs_'}", flush=True)
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
            print(output.decode(errors="replace") if isinstance(output, bytes) else output, end="")
            print(f"Simulator smoke test timed out after {args.timeout}s", file=sys.stderr)
            return 1

    print(proc.stdout, end="")

    if proc.returncode != 0:
        print(f"Simulator smoke test failed with exit code {proc.returncode}", file=sys.stderr)
        return proc.returncode

    for pattern in CRASH_PATTERNS:
        if pattern in proc.stdout:
            print(f"Simulator smoke test output contained crash pattern: {pattern}", file=sys.stderr)
            return 2

    if "Simulator smoke test passed" not in proc.stdout:
        print("Simulator smoke test did not print its success marker", file=sys.stderr)
        return 2

    if (args.performance_report or args.performance_baseline) and not check_lookup_performance(proc.stdout, args):
        return 2
    return 0


def check_lookup_performance(output: str, args: argparse.Namespace) -> bool:
    samples = [int(value) for value in re.findall(r"LOOKUP_PERF redraw_us=(\d+)", output)]
    ready = [int(value) for value in re.findall(r"Dictionary first definition ready after (\d+) ms", output)]
    if len(samples) != 12 or not ready:
        print("Lookup performance coverage incomplete", file=sys.stderr)
        return False
    report = {"env": args.env, "redraw_samples": len(samples),
              "redraw_median_us": statistics.median(samples), "redraw_max_us": max(samples),
              "first_definition_max_ms": max(ready)}
    print("Lookup performance: " + json.dumps(report, sort_keys=True))
    if args.performance_report:
        Path(args.performance_report).write_text(json.dumps(report, indent=2) + "\n")
    if args.performance_baseline:
        baseline = json.loads(Path(args.performance_baseline).read_text())
        if baseline["env"] != args.env:
            print("Lookup baseline device does not match", file=sys.stderr)
            return False
        # Allow 25% growth or a 2 ms host scheduling noise floor. Compare
        # medians; report the maximum separately, since hosts are not real-time.
        limit = max(baseline["redraw_median_us"] * 1.25, baseline["redraw_median_us"] + 2000)
        if report["redraw_median_us"] > limit:
            print(f"Lookup redraw regression: median exceeds {limit:.0f} us", file=sys.stderr)
            return False
        if "first_definition_max_ms" in baseline:
            ready_limit = max(baseline["first_definition_max_ms"] * 1.25,
                              baseline["first_definition_max_ms"] + 20)
            if report["first_definition_max_ms"] > ready_limit:
                print(f"Lookup readiness regression: exceeds {ready_limit:.0f} ms", file=sys.stderr)
                return False
    if "Dictionary first definition missed" in output or "Dictionary initial burst reached" in output:
        print("Lookup exceeded its existing readiness deadline", file=sys.stderr)
        return False
    return True


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--book", default=str(DEFAULT_BOOK), help="EPUB fixture to copy into the isolated simulator fs_")
    parser.add_argument("--env", choices=("simulator", "sticky-simulator", "x4-pro-simulator"), default="simulator",
                        help="PlatformIO simulator environment to build and run")
    parser.add_argument("--timeout", type=int, default=45, help="Seconds before the simulator run is treated as hung")
    parser.add_argument("--page-turns", type=int, default=2, help="Number of EPUB page-forward taps to run")
    parser.add_argument("--theme", choices=sorted(THEMES), help="UI theme to use during the smoke test")
    parser.add_argument("--no-build", dest="build", action="store_false", help="Run the existing simulator binary")
    parser.add_argument("--window", dest="headless", action="store_false", help="Show the SDL window instead of using dummy video")
    parser.add_argument("--lookup-only", action="store_true", help="Isolate dictionary panel checks from touch entry/menu routing")
    parser.add_argument("--performance-report", help="Write dictionary readiness and completed redraw timings as JSON")
    parser.add_argument("--performance-baseline", help="Compare redraw timings against a report from this device profile")
    parser.set_defaults(build=True, headless=True)
    return parser.parse_args()


def main() -> int:
    return run_smoke(parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
