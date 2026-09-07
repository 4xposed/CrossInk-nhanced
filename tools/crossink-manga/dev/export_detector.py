#!/usr/bin/env python3
"""Export Mokuro's pinned comic-text detector for native ONNX Runtime use.

The loaded model and architecture come from comic_text_detector (GPL-3.0).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import onnx
import torch


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("weights", type=Path, help="comictextdetector.pt")
    parser.add_argument("output", type=Path, help="destination .onnx")
    parser.add_argument(
        "--upstream",
        type=Path,
        required=True,
        help="checkout containing the comic_text_detector package",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if not args.weights.is_file():
        raise SystemExit(f"missing detector weights: {args.weights}")
    if not (args.upstream / "comic_text_detector" / "basemodel.py").is_file():
        raise SystemExit(f"invalid comic_text_detector checkout: {args.upstream}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(args.upstream))

    from comic_text_detector.basemodel import TextDetBase

    model = TextDetBase(str(args.weights), device="cpu", act="leaky").eval()
    sample = torch.zeros((1, 3, 1024, 1024), dtype=torch.float32)
    with torch.inference_mode():
        torch.onnx.export(
            model,
            sample,
            str(args.output),
            input_names=["images"],
            output_names=["blocks", "mask", "lines"],
            opset_version=17,
            do_constant_folding=True,
            dynamo=False,
        )

    exported = onnx.load(str(args.output))
    onnx.checker.check_model(exported, full_check=True)
    output_names = [output.name for output in exported.graph.output]
    if output_names != ["blocks", "mask", "lines"]:
        raise SystemExit(f"unexpected detector outputs: {output_names}")
    print(f"Exported {args.output} ({args.output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
