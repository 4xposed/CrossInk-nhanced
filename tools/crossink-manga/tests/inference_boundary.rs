#![allow(clippy::unwrap_used)] // Match the library's test-only assertion convention.

use anyhow::{Context, Result};
use crossink_manga::{
    inference::{Inputs, Outputs, Session},
    native_detector::Detector,
};
use ndarray::{ArrayD, ArrayViewD, IxDyn};

struct InvalidDetector;
struct InvalidOutputs(ArrayD<f32>);

impl Outputs for InvalidOutputs {
    fn len(&self) -> usize {
        1
    }
    fn array(&self, index: usize) -> Result<ArrayViewD<'_, f32>> {
        anyhow::ensure!(index == 0, "missing output");
        Ok(self.0.view())
    }
    fn named_array(&self, name: &str) -> Result<ArrayViewD<'_, f32>> {
        anyhow::ensure!(name == "blocks", "missing output {name}");
        self.array(0)
    }
}

impl Session for InvalidDetector {
    fn run(&mut self, inputs: Inputs<'_>) -> Result<Box<dyn Outputs + '_>> {
        let (name, _) = inputs.first().context("missing detector input")?;
        anyhow::ensure!(*name == "images", "wrong detector input");
        Ok(Box::new(InvalidOutputs(ArrayD::zeros(IxDyn(&[1])))))
    }
}

#[test]
fn detector_validates_alternate_runtime_output_without_loading_ort() {
    let mut detector = Detector::from_session(Box::new(InvalidDetector));
    let error = detector.detect(&image::RgbImage::new(2, 2)).unwrap_err();
    assert!(
        error.to_string().contains("blocks must be rank 3"),
        "{error:#}"
    );
}

struct ScriptedRuntime;
struct ScriptedSession {
    name: String,
    runs: usize,
}
struct ScriptedOutputs(Vec<ArrayD<f32>>);

impl crossink_manga::inference::Runtime for ScriptedRuntime {
    fn cache_identity(&self) -> Result<String> {
        Ok("scripted-test-v1".into())
    }
    fn load(&self, path: &std::path::Path) -> Result<Box<dyn Session>> {
        Ok(Box::new(ScriptedSession {
            name: path.file_name().unwrap().to_str().unwrap().into(),
            runs: 0,
        }))
    }
}

impl Outputs for ScriptedOutputs {
    fn len(&self) -> usize {
        self.0.len()
    }
    fn array(&self, index: usize) -> Result<ArrayViewD<'_, f32>> {
        Ok(self.0.get(index).context("missing output")?.view())
    }
    fn named_array(&self, _: &str) -> Result<ArrayViewD<'_, f32>> {
        anyhow::bail!("scripted recognizer uses declared output order")
    }
}

impl Session for ScriptedSession {
    fn run(&mut self, inputs: Inputs<'_>) -> Result<Box<dyn Outputs + '_>> {
        use crossink_manga::inference::Input;
        if self.name == "encoder.onnx" {
            anyhow::ensure!(
                inputs.len() == 1 && inputs[0].0 == "pixel_values",
                "missing pixels"
            );
            return Ok(Box::new(ScriptedOutputs(vec![ArrayD::from_shape_vec(
                IxDyn(&[1, 2, 3]),
                vec![1., 2., 3., 4., 5., 6.],
            )?])));
        }
        let initial = self.name == "decoder_init.onnx";
        let names: Vec<_> = inputs.iter().map(|(name, _)| *name).collect();
        let expected_names = if initial {
            vec!["input_ids", "encoder_hidden_states"]
        } else {
            vec![
                "input_ids",
                "encoder_hidden_states",
                "past_key_0",
                "past_value_0",
                "past_key_1",
                "past_value_1",
            ]
        };
        anyhow::ensure!(
            names == expected_names,
            "incorrect decoder input names: {names:?}"
        );
        let Input::I64(ids) = &inputs[0].1 else {
            anyhow::bail!("token IDs must be int64")
        };
        let batch = if initial { 1 } else { 4 };
        anyhow::ensure!(ids.shape() == [batch, 1], "incorrect beam batch");
        let expected_ids: &[i64] = match (initial, self.runs) {
            (true, 0) => &[2],
            (false, 0) => &[4, 5, 6, 7],
            (false, 1) => &[6, 7, 4, 0],
            _ => anyhow::bail!("unexpected decoding step"),
        };
        anyhow::ensure!(
            ids.as_slice() == Some(expected_ids),
            "incorrect beam tokens"
        );
        let Input::F32(hidden) = &inputs[1].1 else {
            anyhow::bail!("hidden states must be float32")
        };
        anyhow::ensure!(
            hidden.shape() == [batch, 2, 3],
            "incorrect encoder-state batch"
        );
        let expected_hidden: Vec<f32> = (0..batch).flat_map(|_| [1., 2., 3., 4., 5., 6.]).collect();
        anyhow::ensure!(
            hidden.as_slice() == Some(expected_hidden.as_slice()),
            "encoder states changed"
        );
        if !initial {
            // The second step deliberately selects parents in a different order.
            let parents = if self.runs == 0 {
                [0, 0, 0, 0]
            } else {
                [1, 2, 3, 0]
            };
            for (tensor_index, (_, input)) in inputs.iter().skip(2).enumerate() {
                let Input::F32(cache) = input else {
                    anyhow::bail!("cache must be float32")
                };
                anyhow::ensure!(cache.shape() == [4, 1, 1, 2], "incorrect cache shape");
                for (row, parent) in parents.into_iter().enumerate() {
                    for element in 0..2 {
                        let expected = (tensor_index * 1000 + parent * 10 + element + 1) as f32;
                        anyhow::ensure!(
                            cache[[row, 0, 0, element]] == expected,
                            "wrong cache tensor or beam parent: tensor={tensor_index}, row={row}"
                        );
                    }
                }
            }
        }
        let mut logits = ArrayD::from_elem(IxDyn(&[batch, 1, 8]), -100.0);
        if initial {
            for (rank, token) in [4, 5, 6, 7].into_iter().enumerate() {
                logits[[0, 0, token]] = -(rank as f32) / 10.0;
            }
        } else if self.runs == 0 {
            // A has a uniform continuation; B->C, C->D and D->A overtake it.
            for token in 0..8 {
                logits[[0, 0, token]] = 0.0;
            }
            for (row, token) in [(1, 6), (2, 7), (3, 4)] {
                logits[[row, 0, token]] = 0.0;
            }
        } else {
            // These logits are reached only if the supplied cache state is correct.
            for row in 0..batch {
                logits[[row, 0, 3]] = 0.0;
            }
        }
        let mut outputs = vec![logits];
        for tensor_index in 0..4 {
            outputs.push(ArrayD::from_shape_fn(IxDyn(&[batch, 1, 1, 2]), |index| {
                (tensor_index * 1000 + index[0] * 10 + index[3] + 1) as f32
            }));
        }
        self.runs += 1;
        Ok(Box::new(ScriptedOutputs(outputs)))
    }
}

#[test]
fn recognizer_runs_shared_beam_search_with_an_alternate_runtime() {
    let models = tempfile::tempdir().unwrap();
    std::fs::write(
        models.path().join("recognizer.json"),
        serde_json::to_vec(&serde_json::json!({
            "format_version":1,"input_width":2,"input_height":2,
            "image_mean":[0.5,0.5,0.5],"image_std":[0.5,0.5,0.5],
            "decoder_start_token_id":2,"eos_token_id":3,"pad_token_id":0,
            "max_length":5,"vocab_size":8,"num_beams":4,"length_penalty":2.0,
            "no_repeat_ngram_size":3,"early_stopping":true
        }))
        .unwrap(),
    )
    .unwrap();
    std::fs::write(
        models.path().join("vocab.txt"),
        "[PAD]\n[UNK]\n[CLS]\n[SEP]\nA\nB\nC\nD\n",
    )
    .unwrap();
    for name in ["encoder.onnx", "decoder_init.onnx", "decoder.onnx"] {
        std::fs::write(models.path().join(name), []).unwrap();
    }
    let mut recognizer = crossink_manga::native_recognizer::Recognizer::load_with_runtime(
        models.path(),
        &ScriptedRuntime,
    )
    .unwrap();
    assert_eq!(
        recognizer.recognize(&image::RgbImage::new(2, 2)).unwrap(),
        "ＢＣ"
    );
}

#[test]
#[ignore = "requires ORT_DYLIB_PATH pointing to the native runtime"]
fn ort_adapter_preserves_input_types_and_output_names_and_order() {
    use crossink_manga::inference::{Runtime, Tensor, ort::OrtRuntime};
    let dir = tempfile::tempdir().unwrap();
    let model = dir.path().join("identity.onnx");
    std::fs::write(&model, include_bytes!("fixtures/runtime-identity.onnx")).unwrap();
    let mut session = OrtRuntime.load(&model).unwrap();
    let floats = Tensor::from_array(([2], vec![0.25_f32, -3.0])).unwrap();
    let ids = Tensor::from_array(([2], vec![2_i64, 7])).unwrap();
    let outputs = session
        .run(vec![("pixels", floats.input()), ("ids", ids.input())])
        .unwrap();
    assert_eq!(outputs.array(0).unwrap().as_slice().unwrap(), &[0.25, -3.0]);
    assert_eq!(
        outputs
            .named_array("tokens_float")
            .unwrap()
            .as_slice()
            .unwrap(),
        &[2.0, 7.0]
    );
    assert!(outputs.array(2).is_err());
    assert!(outputs.named_array("absent").is_err());
}

#[test]
#[ignore = "requires ORT_DYLIB_PATH pointing to the native runtime"]
fn ort_relative_override_ignores_conflicting_library_beside_executable() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let working = directory.path().join("working");
    let executable = directory.path().join("executable");
    std::fs::create_dir(&working)?;
    std::fs::create_dir(&executable)?;
    let library =
        std::fs::canonicalize(std::env::var_os("ORT_DYLIB_PATH").context("runtime missing")?)?;
    let library_name = library.file_name().context("runtime filename missing")?;
    std::fs::copy(&library, working.join(library_name))?;
    // Before canonicalization ORT prefers this invalid library, while the cache
    // fingerprint reads the valid one in the working directory.
    std::fs::write(executable.join(library_name), b"not a dynamic library")?;
    let runner = executable.join(format!("adapter-test{}", std::env::consts::EXE_SUFFIX));
    std::fs::copy(std::env::current_exe()?, &runner)?;
    let output = std::process::Command::new(runner)
        .args([
            "--ignored",
            "--exact",
            "ort_adapter_preserves_input_types_and_output_names_and_order",
            "--nocapture",
        ])
        .current_dir(working)
        .env("ORT_DYLIB_PATH", library_name)
        .output()?;
    anyhow::ensure!(
        output.status.success(),
        "relative override failed: {}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    Ok(())
}

#[test]
#[ignore = "requires ORT_DYLIB_PATH pointing to the native runtime"]
fn ort_rejects_initialization_outside_the_adapter() -> Result<()> {
    const CHILD: &str = "CROSSINK_TEST_EXTERNAL_ORT_INIT";
    if std::env::var_os(CHILD).is_some() {
        use crossink_manga::inference::{Runtime, ort::OrtRuntime};
        let library =
            std::fs::canonicalize(std::env::var_os("ORT_DYLIB_PATH").context("runtime missing")?)?;
        ort::init_from(library.to_str().context("runtime path is not UTF-8")?).commit()?;
        let result = OrtRuntime.load(std::path::Path::new("unused.onnx"));
        let error = result
            .err()
            .context("external initialization was silently accepted")?;
        anyhow::ensure!(
            format!("{error:#}").contains("initialized outside this adapter"),
            "{error:#}"
        );
        return Ok(());
    }
    // ORT globals are process-wide; isolate this scenario from the other tests.
    let output = std::process::Command::new(std::env::current_exe()?)
        .args([
            "--ignored",
            "--exact",
            "ort_rejects_initialization_outside_the_adapter",
            "--nocapture",
        ])
        .env(CHILD, "1")
        .output()?;
    anyhow::ensure!(
        output.status.success(),
        "external initialization check failed: {}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    Ok(())
}

struct FullSequenceRuntime;
struct FullSequenceSession {
    encoder: bool,
    calls: usize,
}
impl crossink_manga::inference::Runtime for FullSequenceRuntime {
    fn cache_identity(&self) -> Result<String> {
        Ok("full-sequence-test-v1".into())
    }
    fn load(&self, path: &std::path::Path) -> Result<Box<dyn Session>> {
        anyhow::ensure!(
            path.file_name().unwrap() != "decoder_init.onnx",
            "uncached models have no initial decoder"
        );
        Ok(Box::new(FullSequenceSession {
            encoder: path.file_name().unwrap() == "encoder.onnx",
            calls: 0,
        }))
    }
}
impl Session for FullSequenceSession {
    fn run(&mut self, inputs: Inputs<'_>) -> Result<Box<dyn Outputs + '_>> {
        use crossink_manga::inference::Input;
        if self.encoder {
            return Ok(Box::new(ScriptedOutputs(vec![ArrayD::zeros(IxDyn(&[
                1, 2, 3,
            ]))])));
        }
        anyhow::ensure!(
            inputs.len() == 2,
            "uncached decoder must not receive past-key values"
        );
        let Input::I64(ids) = &inputs[0].1 else {
            anyhow::bail!("wrong token type")
        };
        let expected: &[i64] = match self.calls {
            0 => &[2],
            1 => &[2, 4, 2, 5, 2, 6, 2, 7],
            2 => &[2, 5, 6, 2, 6, 7, 2, 7, 4, 2, 4, 0],
            _ => anyhow::bail!("unexpected step"),
        };
        anyhow::ensure!(
            ids.as_slice() == Some(expected),
            "full prefixes must follow selected beam parents: {ids:?}"
        );
        let batch = if self.calls == 0 { 1 } else { 4 };
        let sequence = self.calls + 1;
        anyhow::ensure!(
            ids.shape() == [batch, sequence],
            "incorrect full-prefix shape"
        );
        let Input::F32(hidden) = &inputs[1].1 else {
            anyhow::bail!("wrong hidden type")
        };
        anyhow::ensure!(
            hidden.shape() == [batch, 2, 3],
            "incorrect encoder-state batch"
        );
        let mut logits = ArrayD::from_elem(IxDyn(&[batch, sequence, 8]), -100.0);
        // Earlier-position logits deliberately disagree with the final position.
        for row in 0..batch {
            for pos in 0..sequence - 1 {
                logits[[row, pos, 7]] = 100.0;
            }
        }
        match self.calls {
            0 => {
                for (rank, token) in [4, 5, 6, 7].into_iter().enumerate() {
                    logits[[0, 0, token]] = -(rank as f32) / 10.0;
                }
            }
            1 => {
                for token in 0..8 {
                    logits[[0, 1, token]] = 0.0;
                }
                for (row, token) in [(1, 6), (2, 7), (3, 4)] {
                    logits[[row, 1, token]] = 0.0;
                }
            }
            _ => {
                for row in 0..batch {
                    logits[[row, sequence - 1, 3]] = 0.0;
                }
            }
        }
        self.calls += 1;
        Ok(Box::new(ScriptedOutputs(vec![logits])))
    }
}
#[test]
fn recognizer_full_sequence_uses_complete_reordered_prefixes_and_last_position() {
    let models = tempfile::tempdir().unwrap();
    std::fs::write(models.path().join("recognizer.json"),serde_json::to_vec(&serde_json::json!({
        "format_version":1,"decoder_interface":"full-sequence-v1","input_width":2,"input_height":2,
        "image_mean":[0.5,0.5,0.5],"image_std":[0.5,0.5,0.5],"decoder_start_token_id":2,"eos_token_id":3,"pad_token_id":0,
        "max_length":5,"vocab_size":8,"num_beams":4,"length_penalty":2.0,"no_repeat_ngram_size":3,"early_stopping":true
    })).unwrap()).unwrap();
    std::fs::write(
        models.path().join("vocab.txt"),
        "[PAD]\n[UNK]\n[CLS]\n[SEP]\nA\nB\nC\nD\n",
    )
    .unwrap();
    for name in ["encoder.onnx", "decoder.onnx"] {
        std::fs::write(models.path().join(name), []).unwrap();
    }
    let mut recognizer = crossink_manga::native_recognizer::Recognizer::load_with_runtime(
        models.path(),
        &FullSequenceRuntime,
    )
    .unwrap();
    assert_eq!(
        recognizer.recognize(&image::RgbImage::new(2, 2)).unwrap(),
        "ＢＣ"
    );
}
