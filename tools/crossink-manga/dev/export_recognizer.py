#!/usr/bin/env python3
"""Export the pinned manga-ocr encoder and decoder for the native Rust backend."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
import torch
from PIL import Image
from transformers import AutoTokenizer, ViTImageProcessor, VisionEncoderDecoderModel
from transformers.cache_utils import DynamicCache, EncoderDecoderCache


MODEL_ID = "kha-white/manga-ocr-base"
MAX_LENGTH = 300


class Encoder(torch.nn.Module):
    def __init__(self, model: VisionEncoderDecoderModel) -> None:
        super().__init__()
        self.encoder = model.encoder

    def forward(self, pixel_values: torch.Tensor) -> torch.Tensor:
        return self.encoder(pixel_values=pixel_values).last_hidden_state


def decoder_outputs(output) -> tuple[torch.Tensor, ...]:
    layers = output.past_key_values.self_attention_cache.layers
    return (
        output.logits,
        layers[0].keys,
        layers[0].values,
        layers[1].keys,
        layers[1].values,
    )


class InitialDecoder(torch.nn.Module):
    def __init__(self, model: VisionEncoderDecoderModel) -> None:
        super().__init__()
        self.decoder = model.decoder

    def forward(
        self, input_ids: torch.Tensor, encoder_hidden_states: torch.Tensor
    ) -> tuple[torch.Tensor, ...]:
        output = self.decoder(
            input_ids=input_ids,
            encoder_hidden_states=encoder_hidden_states,
            use_cache=True,
            return_dict=True,
        )
        return decoder_outputs(output)


class DecoderStep(torch.nn.Module):
    def __init__(self, model: VisionEncoderDecoderModel) -> None:
        super().__init__()
        self.decoder = model.decoder

    def forward(
        self,
        input_ids: torch.Tensor,
        encoder_hidden_states: torch.Tensor,
        past_key_0: torch.Tensor,
        past_value_0: torch.Tensor,
        past_key_1: torch.Tensor,
        past_value_1: torch.Tensor,
    ) -> tuple[torch.Tensor, ...]:
        self_cache = DynamicCache(
            [(past_key_0, past_value_0), (past_key_1, past_value_1)]
        )
        cross_cache = DynamicCache()
        cache = EncoderDecoderCache(self_cache, cross_cache)
        output = self.decoder(
            input_ids=input_ids,
            encoder_hidden_states=encoder_hidden_states,
            past_key_values=cache,
            use_cache=True,
            return_dict=True,
        )
        return decoder_outputs(output)


def export_graphs(model: VisionEncoderDecoderModel, output: Path) -> None:
    pixels = torch.zeros((1, 3, 224, 224), dtype=torch.float32)
    with torch.inference_mode():
        hidden = model.encoder(pixel_values=pixels).last_hidden_state
    # Tracing records decoder operations, so its sample must be an ordinary tensor.
    hidden = hidden.clone()

    torch.onnx.export(
        Encoder(model).eval(),
        (pixels,),
        output / "encoder.onnx",
        input_names=["pixel_values"],
        output_names=["encoder_hidden_states"],
        dynamic_axes=None,
        opset_version=17,
        do_constant_folding=True,
        dynamo=False,
    )
    sample_ids = torch.tensor([[model.config.decoder_start_token_id]], dtype=torch.int64)
    torch.onnx.export(
        InitialDecoder(model).eval(),
        (sample_ids, hidden),
        output / "decoder_init.onnx",
        input_names=["input_ids", "encoder_hidden_states"],
        output_names=["logits", "present_key_0", "present_value_0", "present_key_1", "present_value_1"],
        dynamic_axes={
            "input_ids": {0: "batch"}, "encoder_hidden_states": {0: "batch"},
            "logits": {0: "batch"},
            "present_key_0": {0: "batch"}, "present_value_0": {0: "batch"},
            "present_key_1": {0: "batch"}, "present_value_1": {0: "batch"},
        },
        opset_version=17,
        do_constant_folding=True,
        dynamo=False,
    )
    with torch.inference_mode():
        initial = InitialDecoder(model)(sample_ids, hidden)
    torch.onnx.export(
        DecoderStep(model).eval(),
        (sample_ids, hidden, *[value.clone() for value in initial[1:]]),
        output / "decoder.onnx",
        input_names=[
            "input_ids", "encoder_hidden_states",
            "past_key_0", "past_value_0", "past_key_1", "past_value_1",
        ],
        output_names=["logits", "present_key_0", "present_value_0", "present_key_1", "present_value_1"],
        dynamic_axes={
            "input_ids": {0: "batch"}, "encoder_hidden_states": {0: "batch"},
            "logits": {0: "batch"},
            "past_key_0": {0: "batch", 2: "past_sequence"},
            "past_value_0": {0: "batch", 2: "past_sequence"},
            "past_key_1": {0: "batch", 2: "past_sequence"},
            "past_value_1": {0: "batch", 2: "past_sequence"},
            "present_key_0": {0: "batch", 2: "present_sequence"},
            "present_value_0": {0: "batch", 2: "present_sequence"},
            "present_key_1": {0: "batch", 2: "present_sequence"},
            "present_value_1": {0: "batch", 2: "present_sequence"},
        },
        opset_version=17,
        do_constant_folding=True,
        dynamo=False,
    )
    # Keep the shared submodules deterministic after the exporter restores state.
    model.eval()


def greedy_onnx(
    image: Image.Image,
    processor: ViTImageProcessor,
    encoder: ort.InferenceSession,
    decoder_init: ort.InferenceSession,
    decoder: ort.InferenceSession,
    start: int,
    eos: int,
    max_length: int,
) -> list[int]:
    rgb = image.convert("L").convert("RGB")
    pixels = processor(rgb, return_tensors="np").pixel_values.astype(np.float32)
    hidden = encoder.run(None, {"pixel_values": pixels})[0]
    tokens = [start]
    outputs = decoder_init.run(
        None,
        {"input_ids": np.asarray([[start]], dtype=np.int64), "encoder_hidden_states": hidden},
    )
    while len(tokens) < max_length:
        token = int(np.argmax(outputs[0][0, -1]))
        if token == eos:
            break
        tokens.append(token)
        if len(tokens) >= max_length:
            break
        logits = decoder.run(
            None,
            {
                "input_ids": np.asarray([[token]], dtype=np.int64),
                "encoder_hidden_states": hidden,
                "past_key_0": outputs[1], "past_value_0": outputs[2],
                "past_key_1": outputs[3], "past_value_1": outputs[4],
            },
        )
        outputs = logits
    return tokens


def validate_cached_steps(
    model: VisionEncoderDecoderModel,
    hidden: np.ndarray,
    decoder_init: ort.InferenceSession,
    decoder: ort.InferenceSession,
    steps: int = 8,
) -> None:
    start = model.config.decoder_start_token_id
    token = start
    with torch.inference_mode():
        expected = model.decoder(
            input_ids=torch.tensor([[start]], dtype=torch.int64),
            encoder_hidden_states=torch.from_numpy(hidden),
            use_cache=True,
        )
    actual = decoder_init.run(
        None,
        {"input_ids": np.asarray([[start]], dtype=np.int64), "encoder_hidden_states": hidden},
    )
    for _past_length in range(1, steps + 1):
        np.testing.assert_allclose(actual[0], expected.logits.numpy(), rtol=3e-4, atol=3e-4)
        expected_layers = expected.past_key_values.self_attention_cache.layers
        for actual_cache, expected_cache in zip(
            actual[1:],
            (
                expected_layers[0].keys,
                expected_layers[0].values,
                expected_layers[1].keys,
                expected_layers[1].values,
            ),
        ):
            np.testing.assert_allclose(
                actual_cache, expected_cache.numpy(), rtol=3e-4, atol=3e-4
            )
        token = int(np.argmax(expected.logits.numpy()[0, -1]))
        if token == model.config.eos_token_id:
            break
        with torch.inference_mode():
            expected = model.decoder(
                input_ids=torch.tensor([[token]], dtype=torch.int64),
                encoder_hidden_states=torch.from_numpy(hidden),
                past_key_values=expected.past_key_values,
                use_cache=True,
            )
        actual = decoder.run(
            None,
            {
                "input_ids": np.asarray([[token]], dtype=np.int64),
                "encoder_hidden_states": hidden,
                "past_key_0": actual[1], "past_value_0": actual[2],
                "past_key_1": actual[3], "past_value_1": actual[4],
            },
        )


def validate(
    model: VisionEncoderDecoderModel,
    processor: ViTImageProcessor,
    output: Path,
    image_path: Path | None,
) -> None:
    onnx.checker.check_model(output / "encoder.onnx")
    onnx.checker.check_model(output / "decoder_init.onnx")
    onnx.checker.check_model(output / "decoder.onnx")
    encoder = ort.InferenceSession(output / "encoder.onnx", providers=["CPUExecutionProvider"])
    decoder_init = ort.InferenceSession(output / "decoder_init.onnx", providers=["CPUExecutionProvider"])
    decoder = ort.InferenceSession(output / "decoder.onnx", providers=["CPUExecutionProvider"])

    pixels = np.zeros((1, 3, 224, 224), dtype=np.float32)
    with torch.inference_mode():
        expected_hidden = model.encoder(torch.from_numpy(pixels)).last_hidden_state.numpy()
        validation_ids = torch.tensor([[model.config.decoder_start_token_id]], dtype=torch.int64)
        expected_init = model.decoder(
            input_ids=validation_ids,
            encoder_hidden_states=torch.from_numpy(expected_hidden),
            use_cache=True,
        )
    actual_hidden = encoder.run(None, {"pixel_values": pixels})[0]
    actual_init = decoder_init.run(
        None,
        {
            "input_ids": validation_ids.numpy(),
            "encoder_hidden_states": actual_hidden,
        },
    )
    np.testing.assert_allclose(actual_hidden, expected_hidden, rtol=2e-4, atol=2e-4)
    np.testing.assert_allclose(actual_init[0], expected_init.logits.numpy(), rtol=3e-4, atol=3e-4)
    validate_cached_steps(model, actual_hidden, decoder_init, decoder)

    if image_path is not None:
        image = Image.open(image_path)
        rgb = image.convert("L").convert("RGB")
        pixels = processor(rgb, return_tensors="pt").pixel_values
        with torch.inference_mode():
            expected = model.generate(
                pixels,
                max_length=MAX_LENGTH,
                num_beams=1,
                no_repeat_ngram_size=0,
            )[0].tolist()
        actual = greedy_onnx(
            image,
            processor,
            encoder,
            decoder_init,
            decoder,
            model.config.decoder_start_token_id,
            model.config.eos_token_id,
            MAX_LENGTH,
        )
        # generate includes the terminal separator while the runtime stops before it.
        if expected and expected[-1] == model.config.eos_token_id:
            expected = expected[:-1]
        if actual != expected:
            raise RuntimeError(f"ONNX greedy tokens differ: ONNX={actual!r}, PyTorch={expected!r}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", default=MODEL_ID, help="local path or Hugging Face model ID")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--image", type=Path, help="optional real image for exact greedy-token validation")
    parser.add_argument("--offline", action="store_true", help="read model files only from the Hugging Face cache")
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    model = VisionEncoderDecoderModel.from_pretrained(
        args.model, local_files_only=args.offline
    )
    # SDPA's causal branch is specialized to the trace example length. Eager
    # attention exports the decoder's dynamic causal mask as ordinary ONNX ops.
    model.set_attn_implementation("eager")
    model.eval()
    processor = ViTImageProcessor.from_pretrained(
        args.model, local_files_only=args.offline
    )
    tokenizer = AutoTokenizer.from_pretrained(
        args.model, tokenizer_type="bert-japanese", local_files_only=args.offline
    )

    export_graphs(model, args.output)
    saved_vocab = tokenizer.save_vocabulary(args.output)
    if Path(saved_vocab[0]).name != "vocab.txt":
        raise RuntimeError(f"tokenizer wrote an unexpected vocabulary path: {saved_vocab!r}")
    config = {
        "format_version": 1,
        "input_width": 224,
        "input_height": 224,
        "image_mean": list(processor.image_mean),
        "image_std": list(processor.image_std),
        "decoder_start_token_id": model.config.decoder_start_token_id,
        "eos_token_id": model.config.eos_token_id,
        "pad_token_id": model.config.pad_token_id,
        "max_length": MAX_LENGTH,
        "vocab_size": model.config.decoder.vocab_size,
        "num_beams": model.generation_config.num_beams,
        "length_penalty": model.generation_config.length_penalty,
        "no_repeat_ngram_size": model.generation_config.no_repeat_ngram_size,
        "early_stopping": model.generation_config.early_stopping,
    }
    (args.output / "recognizer.json").write_text(
        json.dumps(config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    validate(model, processor, args.output, args.image)
    print(f"exported and validated recognizer models in {args.output}")


if __name__ == "__main__":
    main()
