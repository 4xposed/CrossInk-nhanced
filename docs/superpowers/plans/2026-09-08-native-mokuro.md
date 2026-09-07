# Native Mokuro implementation plan

User-approved goal: replace runtime Python OCR with native Rust inference and demonstrate comparable results against upstream on `/Users/daniel/Downloads/Kawazuya v01.cbz`.

Architecture: retain shared original-resolution panel preparation and CMI2 export; native detector and recognizer run the existing trained models through ONNX Runtime. Development-only Python may export models and generate reference fixtures. Upstream remains an explicit reference backend during comparison.

Constraints: no commits/pushes; no manga images or full OCR text added to repository; local comparison artifacts only. Preserve prior uncommitted work. Test first, retain failure evidence, and report actual metrics rather than claiming parity from successful execution. No automatic upstream fallback in native mode.

## Tasks

- [x] Recognition: encoder/decoder export, Rust preprocessing/token generation/normalization, independent tests and same-crop reference comparison. Worker mokuro_io owns native_recognizer.rs and dev/export_recognizer.py.
- [x] Detection: detector export, native region/line geometry and reading order, original-resolution line crops, independent tests. Worker mokuro_firmware owns native_detector.rs and dev/export_detector.py.
- [x] Integration: root owns Cargo, native orchestration, CLI backend/model options, resumable per-page OCR, comparison harness and deterministic matching tests.
- [x] Acceptance: run both backends on identical prepared panels from all 185 source images; record page coverage, region matching and character disagreement, timings and outliers. Test native model/session loading and final CMI output independently.
- [x] Packaging/docs: include native runtime and model acquisition path without runtime Python; update workflow/docs; independent review and fix findings.

## Execution ledger

Same-checkout extension of approved unfinished feature; keep source CBZ untouched. Reuse existing agent slots because the session has a four-agent limit. Disjoint detector/recognizer files permit independent work; root owns every shared interface/dependency. Model export scripts are development tooling, never called by native conversion.

Initial comparison criteria (set before results): 100% same-panel coverage; report unmatched blocks explicitly; normalized character edit distance on matched blocks plus whole-panel text, and geometry IoU distribution. A provisional target is >=90% reference-region recall and candidate-region precision at IoU >=0.5, and <=10% aggregate normalized character disagreement on both matched blocks and whole-panel text; these measure agreement with upstream, not ground-truth accuracy. Do not relax thresholds silently if missed.

Interfaces: Detector::load(Path), detect(RgbImage)->Vec<DetectedBlock>; each block has bounds, vertical/font_size and lines with quadrilateral and cropped RGB image. Recognizer::load(Path), recognize(RgbImage)->String. Both use ort 2.0.0-rc.10 dynamic runtime initially. Model paths and hashes are recorded in comparison evidence.

Pair checks: detector output line crops feed recognizer RGB input; combined output maps to existing mokuro::Page fields. Neither worker changes Cargo/lib/main. Existing exporter validates schemas and transforms coordinates after OCR. Task tests match these same interfaces.


Progress: upstream reference completed all 431 crops from 185 source images, 579 blocks and 5,049 characters, 197 seconds processing. First native diagnostic run was stopped when tests exposed dynamic decoder causal-mask specialization and detector polygon-scoring differences. Those results are not acceptance evidence. Cache identities now include executable and native runtime hashes as well as model/crop content, preventing stale results across fixes.

Packaging decision: preserve GPL notices for translated detector/Mokuro routines; retain original MIT notices for original converter modules. UnRAR moved to a separate MIT executable exchanging files, so the GPL-derived OCR executable does not statically link it. Native bundle includes runtime, models, provenance/notices and buildable source. This adds one archive helper to the bundle; CBZ processing and OCR remain in the main Rust executable.

Recognition correction: the early assumed greedy-generation contract was wrong. The pinned upstream model defaults to four beams, length penalty 2, and no-repeat trigrams. Native recognition must implement those defaults. Raw greedy per-step logit checks remain diagnostic, not acceptance; the full upstream baseline and thresholds are unchanged.

Geometry compatibility: unmodified upstream produced two edge-crossing boxes and one edge-crossing line polygon. Shared comparison/export now intersects finite positive-area boxes with source bounds, rejects fully invisible/nonfinite rectangles, and records clipping indexes without editing either master. Regression first failed on strict rejection, then passed with clipping. Line polygons are only metadata here and must be finite; exported hit boxes use clipped rectangles.

Detector-only final pass before long-line chunking: 532/579 reference matches, 550 native blocks, recall 91.88%, precision 96.73%, mean IoU .9554. Text agreement still requires the full native run.

First complete native diagnostic: all431panels exported, region recall91.88%, precision96.73%, matched text disagreement13.44%, whole-panel14.62% (738/5049edits). This fails the unchanged10% gate. Outliers identified missing upstream split/order behavior, source-coordinate rounding and warp differences. Original diagnostic retained outside repo; subsequent run must establish acceptance. Independent native/Python same-crop crosschecks isolate crop geometry from recognition.

Warp TDD: independent OpenCV5 synthetic non-affine, edge-crossing quadrilateral fixture first failed against stub and against assumed1/32pixel quantization. Measured installed OpenCV5 uses continuous linear sampling. Closed-form projective mapping + black border matches125/126bytes exactly and one half-byte tie within1 intensity unit. No corpus threshold changed.

Second complete diagnostic after upstream split/order, detector-coordinate rounding and projective warp: recall97.58%, precision99.12%, matched CER9.09% passes; panel CER11.92% (602/5049edits) still fails. Retained outside repo. Remaining measured fixes: exact Pillow separable resize, and raster-boundary tolerance confined to split intersection (each contour may be off1pixel; no metric threshold relaxation).

Runtime portability: actual CLI reproduction with ORT1.22 aborted at Environment destruction after publishing output. ORT1.23.2 completed fresh synthetic inference and normal teardown; official Intel wheel exists. Intel CI nowpins1.23.2; local execution was ARM64 only. Recognition crosscheck after fixed-point resize:43adversarial lines,389referencecharacters,0edits on identical crops. Actual installed Transformers uses Torchvision resizing; documented residual byte-level differences remain, judged by unchanged corpus agreement gate.

Third diagnostic: recall97.24%, precision99.29%, matched CER9.02%, whole-panel10.44% (527/5049), so stillnotaccepted. Identified two real missing regions: upstream foreground-contour means .646315/.604413 versus native row-span means .589953/.580735; native wrongly rejected both at unchanged.6 scorethreshold. Correction uses component pixels. Review also found mask/RGB warp mismatch; new shared projective mapping regression failsbeforefix andpassesafter.

Acceptance achieved on fourth complete native run: all431panels,574matches/579reference/580native, recall99.14%, precision98.97%, matchedCER8.93%, whole-panelCER9.45% (477/5049). Source snapshot remained unchanged throughout run. Original referenceSHAunchanged. Final nativeJSONSHA c1221432de749fc66eeac724a2d4cb7c6afe876a167b2a43575874c2bb6daefc. Packaging/device export verification follows.

Completed: native/upstreamexportsvalidatedforX3,X4,X4Pro,431BMPseachbit-identicalbetweenthebackends. NativeX3exportreused431cachedOCRpages. MacARM64ZIPbuiltwithruntime/models/helper/notices/correspondingsource; extractedbundleperformedrealnativeinferenceandvalidationwithPATHemptyandmodel/runtimeoverridesunset, exit0. Localacceptance.jsonrecordsfinalhashes. Nohardwareexecutionclaimed; otherOS/IntelCIexecutionpending. Nocommits/pushes.
