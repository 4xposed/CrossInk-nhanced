# Upstream components

The native OCR tool includes Rust adaptations of Mokuro 0.2.5 and its bundled comic-text-detector preprocessing, geometry, and grouping routines. These upstream components use GPL-3.0; see LICENSE. Native detector modifications were made for CrossInk in 2026. Original CrossInk converter modules retain their MIT terms (LICENSE-MIT). The separate crossink-rar helper retains MIT terms and its UnRAR dependency notices.

Sources:
- https://github.com/kha-white/mokuro/tree/v0.2.5
- https://github.com/dmMaze/comic-text-detector
- https://github.com/kha-white/manga-ocr (Apache-2.0)
- https://huggingface.co/kha-white/manga-ocr-base (model and tokenizer)
- https://github.com/zyddnys/manga-image-translator/releases/tag/beta-0.2.1 (detector checkpoint used by Mokuro)

Model export scripts under dev/ describe the transformations. Model bundles include checkpoint/export hashes and applicable upstream notices. ONNX Runtime is a separately provided native runtime with its own notices. Release artifacts include buildable tool source and vendored dependency sources. Do not remove notices from downstream distributions.

Recognition preprocessing includes a Rust adaptation of Pillow’s fixed-point bilinear resizing. Pillow retains its HPND license; see third-party-licenses/Pillow-LICENSE (copied into licenses in binary bundles). Source: https://github.com/python-pillow/Pillow/blob/12.3.0/src/libImaging/Resample.c .
