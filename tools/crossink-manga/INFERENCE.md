# Inference runtime boundary

The native CLI uses ONNX Runtime by default. `--backend native|upstream`
continues to select the OCR implementation; it is not an inference runtime flag.

`src/inference.rs` defines `Runtime`, `Session`, `Inputs`, and `Outputs`.
`src/inference/ort.rs` owns all ONNX Runtime initialization, native library lookup,
session options, and ORT tensor conversion. The detector and recognizer own model
names, shapes, preprocessing, geometry, beam search, and text postprocessing.

To add a native runtime:

1. Implement `Runtime::load` and `Runtime::cache_identity` in a new adapter.
2. Implement `Session::run` for named float32/int64 input views. Return float32
   outputs in declared ONNX graph order, with lookup by output name. Missing
   outputs and incompatible types must return errors.
3. Pass the runtime to `native::run_with_runtime`, or the detector/recognizer's
   `load_with_runtime` methods. The existing `run` and `load` entry points keep
   selecting ORT.
4. Include the runtime version, binary identity and execution settings in its
   cache identity. This is combined with models, executable and image identities
   to prevent accidentally reusing results from another runtime.
5. Test the same exported models and fixtures before exposing a CLI selector.
   New runtimes may require different exports; the interface does not guarantee
   operator support or numerical parity.

Sessions and outputs use trait objects to allow runtime selection without making
the model algorithms generic. There is a small allocation for each output owner;
large detector output buffers are borrowed rather than copied by the boundary.
The initial decoder borrows encoder output directly. The recognizer builds its
batched encoder-state buffer once per crop and reuses it throughout decoding;
beam-search cache reordering still uses host copies. These changes reduce buffer
allocation and copying, but have not been benchmarked for end-to-end speed.
Repetition filtering scans each beam's history once to populate a blocked-token
mask, reusing the mask across beams in that decoding step. Candidate ordering and
score normalization are unchanged; exhaustive short-history tests compare the
mask against the original scalar rule, retained only in test builds.

The adapter canonicalizes `ORT_DYLIB_PATH` before hashing or loading it. Relative
overrides refer to the working directory at first resolution, and the resolved
library is fixed for the process lifetime, matching ORT's own behavior. Set the
override before using the adapter. An ORT environment initialized elsewhere is
rejected rather than silently accepting unverified library/settings; restarting
the process is required after initialization failure.

## Browser follow-up

This is a synchronous native CPU interface, not a finished WASM application.
Browser inference needs awaitable execution and output materialization; the file
loader also needs a byte/asset source. GPU-resident decoder state would need an
extended tensor-handle contract instead of host views. Keep those changes at the
inference boundary, and run conversion work in a worker rather than the UI thread.
Native dependencies still need target gating before compiling the full crate for
the browser. No browser runtime, frontend, or second inference backend ships yet.

## Verification

`cargo test --locked` exercises the model algorithms with substitute graph sessions
without a model download or native runtime. To test the real ORT adapter:

```sh
ORT_DYLIB_PATH=/path/to/runtime cargo test --locked --test inference_boundary -- --include-ignored
```

`tests/fixtures/runtime-identity.onnx` is a synthetic, weight-free ONNX IR 8/opset
13 graph created for this test. It passes through two float32 values and casts two
int64 IDs to float32, testing the adapter's input types, output names and ordering.
Full OCR parity against upstream remains a separate model-dependent check.
The integration suite also checks relative library paths with conflicting files
beside the executable, and external ORT initialization in isolated subprocesses.
The substitute recognizer uses distinct cache values and changes beam order so
that swapped cache tensors or incorrect parent selection fail the test.
