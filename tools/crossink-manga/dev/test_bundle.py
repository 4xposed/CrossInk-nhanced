"""Verify extracted native bundle inference with no Python/uv executable on PATH."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument("archive", type=Path)
args = parser.parse_args()
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    with zipfile.ZipFile(args.archive) as archive:
        archive.extractall(root)
        for entry in archive.infolist():
            path = root / entry.filename
            if path.is_file() and os.name != "nt":
                path.chmod((entry.external_attr >> 16) & 0o777)
    bundle = next(path for path in root.iterdir() if path.is_dir())
    executable = bundle / ("crossink-manga.exe" if os.name == "nt" else "crossink-manga")
    fixture = Path(__file__).resolve().parent.parent / "tests/fixtures/native-smoke.png"
    source = root / "synthetic.cbz"
    with zipfile.ZipFile(source, "w") as archive:
        archive.write(fixture, "001.png")
    environment = dict(os.environ)
    environment["PATH"] = ""
    for key in ("ORT_DYLIB_PATH", "CROSSINK_MANGA_MODELS", "CROSSINK_RAR_HELPER"):
        environment.pop(key, None)
    output = root / "book"
    subprocess.run([str(executable), "convert", str(source), "--output", str(output)], env=environment, check=True)
    subprocess.run([str(executable), "validate", str(output)], env=environment, check=True)
    ocr = json.loads((root / "book.work/crops.native.mokuro").read_text())
    if len(ocr["pages"]) != 2 or not all(any(block["lines"] for block in page["blocks"]) for page in ocr["pages"]):
        raise SystemExit("Synthetic fixture did not exercise both detector and recognizer")
    print("Extracted bundle native inference passed without Python/uv on PATH")
