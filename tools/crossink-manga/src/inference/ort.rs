//! Native ONNX Runtime implementation. All `ort` types stay in this module.
use super::{Input, Inputs, Outputs, Runtime, Session};
use ::ort::{
    session::{
        Session as OrtSession, SessionInputValue, SessionOutputs, builder::GraphOptimizationLevel,
    },
    value::TensorRef,
};
use anyhow::ensure;
use anyhow::{Context, Result};
use ndarray::ArrayViewD;
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::Read,
    path::{Path, PathBuf},
    sync::OnceLock,
};

pub struct OrtRuntime;

impl Runtime for OrtRuntime {
    fn cache_identity(&self) -> Result<String> {
        let mut hash = Sha256::new();
        let mut file = fs::File::open(runtime_path()?)?;
        let mut buffer = [0u8; 65536];
        loop {
            let count = file.read(&mut buffer)?;
            if count == 0 {
                break;
            }
            hash.update(&buffer[..count]);
        }
        Ok(format!(
            "ort-rc10-cpu-intra2-inter1-opt3:{:x}",
            hash.finalize()
        ))
    }

    fn load(&self, path: &Path) -> Result<Box<dyn Session>> {
        initialize_runtime()?;
        let session = OrtSession::builder()?
            .with_intra_threads(2)?
            .with_inter_threads(1)?
            .with_optimization_level(GraphOptimizationLevel::Level3)?
            .commit_from_file(path)
            .with_context(|| format!("load ONNX model {}", path.display()))?;
        // One allocation per graph allows selecting implementations at runtime.
        Ok(Box::new(OrtGraph(session)))
    }
}

struct OrtGraph(OrtSession);

impl Session for OrtGraph {
    fn run(&mut self, inputs: Inputs<'_>) -> Result<Box<dyn Outputs + '_>> {
        let inputs = inputs
            .into_iter()
            .map(|(name, input)| {
                let value: SessionInputValue<'_> = match input {
                    Input::F32(array) => TensorRef::from_array_view(array)?.into(),
                    Input::I64(array) => TensorRef::from_array_view(array)?.into(),
                };
                Ok((name, value))
            })
            .collect::<Result<Vec<_>>>()?;
        // Box only the small output owner; tensor data is borrowed, not copied.
        Ok(Box::new(OrtOutputs(self.0.run(inputs)?)))
    }
}

struct OrtOutputs<'a>(SessionOutputs<'a>);

impl Outputs for OrtOutputs<'_> {
    fn len(&self) -> usize {
        self.0.len()
    }
    fn array(&self, index: usize) -> Result<ArrayViewD<'_, f32>> {
        anyhow::ensure!(index < self.0.len(), "missing model output {index}");
        Ok(self.0[index].try_extract_array::<f32>()?)
    }
    fn named_array(&self, name: &str) -> Result<ArrayViewD<'_, f32>> {
        Ok(self
            .0
            .get(name)
            .with_context(|| format!("missing model output {name}"))?
            .try_extract_array::<f32>()?)
    }
}

/// Resolve once: ORT also selects its library once per process.
/// Relative overrides are interpreted against the initial working directory.
pub fn runtime_path() -> Result<PathBuf> {
    static PATH: OnceLock<PathBuf> = OnceLock::new();
    if let Some(path) = PATH.get() {
        return Ok(path.clone());
    }
    let path = if let Some(explicit) = std::env::var_os("ORT_DYLIB_PATH") {
        PathBuf::from(explicit)
    } else {
        let name = if cfg!(target_os = "windows") {
            "onnxruntime.dll"
        } else if cfg!(target_os = "macos") {
            "libonnxruntime.dylib"
        } else {
            "libonnxruntime.so"
        };
        std::env::current_exe()
            .context("locate executable for bundled ONNX runtime")?
            .with_file_name(name)
    };
    let path = canonical_runtime_path(&path)?;
    Ok(PATH.get_or_init(|| path).clone())
}

fn canonical_runtime_path(path: &Path) -> Result<PathBuf> {
    ensure!(
        path.is_file(),
        "native ONNX runtime missing: {}; install the full bundle or set ORT_DYLIB_PATH",
        path.display()
    );
    fs::canonicalize(path).with_context(|| format!("resolve ONNX runtime {}", path.display()))
}

fn initialize_runtime() -> Result<()> {
    static INITIALIZED: OnceLock<std::result::Result<(), String>> = OnceLock::new();
    let result =
        INITIALIZED.get_or_init(|| initialize_runtime_once().map_err(|error| format!("{error:#}")));
    result
        .as_ref()
        .map_err(|error| anyhow::anyhow!("{error}"))?;
    Ok(())
}

fn initialize_runtime_once() -> Result<()> {
    let path = runtime_path()?;
    let path = path.to_str().context("runtime path is not UTF-8")?;
    // ORT pins its library even on failure. Retain the result rather than implying
    // that changing ORT_DYLIB_PATH can recover an already initialized process.
    let configured = std::panic::catch_unwind(|| {
        ort::init_from(path)
            .with_name("crossink-manga")
            .with_telemetry(false)
            .commit()
    })
    .map_err(|_| anyhow::anyhow!("could not load compatible ONNX runtime at {path}"))??;
    ensure!(
        configured,
        "ONNX Runtime was initialized outside this adapter; its library and settings cannot be verified"
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn relative_runtime_path_is_canonical_before_loading_or_hashing() {
        let directory = tempfile::tempdir_in(".").unwrap();
        let relative = directory.path().join("runtime-library");
        fs::write(&relative, b"runtime").unwrap();
        let resolved = canonical_runtime_path(&relative).unwrap();
        assert!(resolved.is_absolute());
        assert_eq!(resolved, fs::canonicalize(relative).unwrap());
    }

    #[test]
    fn runtime_path_rejects_directories_and_missing_files() {
        let directory = tempfile::tempdir().unwrap();
        assert!(canonical_runtime_path(directory.path()).is_err());
        assert!(canonical_runtime_path(&directory.path().join("absent")).is_err());
    }
}
