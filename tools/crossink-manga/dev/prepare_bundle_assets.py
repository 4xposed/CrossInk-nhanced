#!/usr/bin/env python3
"""Development/CI only: export pinned models and collect native bundle inputs."""
import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from huggingface_hub import snapshot_download
import mokuro
from mokuro.cache import cache
import onnxruntime

REVISION = "aa6573bd10b0d446cbf622e29c3e084914df9741"


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--offline", action="store_true", help="use the already cached pinned source model")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    here = Path(__file__).resolve().parent
    upstream = Path(mokuro.__file__).resolve().parent.parent
    weights = Path(cache.comic_text_detector)
    if digest(weights) != "1f90fa60aeeb1eb82e2ac1167a66bf139a8a61b8780acd351ead55268540cccb":
        raise SystemExit("Detector checkpoint does not match the verified upstream fixture")
    snapshot = Path(snapshot_download("kha-white/manga-ocr-base", revision=REVISION,
        local_files_only=args.offline,
        allow_patterns=["*.json", "*.txt", "pytorch_model.bin", "*.safetensors", "README.md", "LICENSE"]))
    subprocess.run([sys.executable, str(here / "export_detector.py"), str(weights), str(output / "comictextdetector.onnx"), "--upstream", str(upstream)], check=True)
    subprocess.run([sys.executable, str(here / "export_recognizer.py"), "--model", str(snapshot), "--output", str(output), "--offline"], check=True)
    shutil.copy2(upstream / "comic_text_detector" / "LICENSE", output / "detector-LICENSE")
    # Preserve the model card and explicit license from the pinned model snapshot.
    for name in ("README.md", "LICENSE"):
        if (snapshot / name).is_file():
            shutil.copy2(snapshot / name, output / ("model-" + name))
    distribution = importlib.metadata.distribution("manga-ocr")
    licenses = [distribution.locate_file(f) for f in distribution.files if str(f).endswith("licenses/LICENSE")]
    if not licenses:
        raise SystemExit("manga-ocr license not found")
    shutil.copy2(licenses[0], output / "recognizer-LICENSE")
    names = ["comictextdetector.onnx", "encoder.onnx", "decoder_init.onnx", "decoder.onnx", "vocab.txt", "recognizer.json"]
    provenance = {
        "recognizer_source": "https://huggingface.co/kha-white/manga-ocr-base",
        "recognizer_revision": REVISION,
        "detector_source": "https://github.com/zyddnys/manga-image-translator/releases/download/beta-0.2.1/comictextdetector.pt",
        "detector_checkpoint_sha256": digest(weights),
        "exports": {name: digest(output / name) for name in names},
        "development_packages": {name: importlib.metadata.version(name) for name in ("mokuro", "manga-ocr", "torch", "transformers", "onnx", "onnxruntime")},
        "export_scripts": {name: digest(here / name) for name in ("export_detector.py", "export_recognizer.py")},
    }
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2))
    ort_root = Path(onnxruntime.__file__).resolve().parent
    binaries = [p for p in (ort_root / "capi").iterdir() if p.name.startswith(("libonnxruntime.", "onnxruntime.dll")) and p.is_file()]
    if len(binaries) != 1:
        raise SystemExit(f"Expected one ONNX Runtime binary, found {binaries}")
    values = {"NATIVE_MODELS_DIR": str(output), "ORT_DYLIB_PATH": str(binaries[0]), "ORT_LICENSE_DIR": str(ort_root)}
    print(json.dumps(values, indent=2))
    if os.environ.get("GITHUB_ENV"):
        with open(os.environ["GITHUB_ENV"], "a") as env:
            for key, value in values.items():
                env.write(f"{key}={value}\n")


if __name__ == "__main__":
    main()
