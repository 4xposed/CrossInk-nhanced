use anyhow::{Context, Result, ensure};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::{Read, Write},
    path::{Path, PathBuf},
};

#[derive(Debug, Default, Clone, Copy, clap::ValueEnum)]
pub enum Backend {
    #[default]
    Native,
    Upstream,
}

pub fn model_directory(explicit: Option<&Path>) -> Result<PathBuf> {
    let path = if let Some(path) = explicit {
        path.to_path_buf()
    } else if let Some(path) = std::env::var_os("CROSSINK_MANGA_MODELS") {
        PathBuf::from(path)
    } else {
        std::env::current_exe()?
            .parent()
            .context("executable parent missing")?
            .join("models")
    };
    ensure!(
        path.is_dir(),
        "native OCR model directory missing: {}; use --models or install the full release bundle",
        path.display()
    );
    Ok(path)
}

fn hash_file(path: &Path, hash: &mut Sha256) -> Result<()> {
    let mut file = fs::File::open(path)?;
    let mut bytes = [0; 65536];
    loop {
        let count = file.read(&mut bytes)?;
        if count == 0 {
            break;
        }
        hash.update(&bytes[..count]);
    }
    Ok(())
}

fn cache_identity(image: &Path, models: &str) -> Result<String> {
    let mut hash = Sha256::new();
    hash.update(b"crossink-native-ocr-v1");
    hash.update(models.as_bytes());
    hash_file(image, &mut hash)?;
    Ok(format!("{:x}", hash.finalize()))
}

fn model_fingerprint(models: &Path, runtime: &dyn crate::inference::Runtime) -> Result<String> {
    let mut files = fs::read_dir(models)?
        .map(|entry| entry.map(|e| e.path()))
        .collect::<std::io::Result<Vec<_>>>()?;
    files.sort();
    let mut hash = Sha256::new();
    // A new implementation or inference runtime must not reuse old OCR during parity work.
    hash_file(&std::env::current_exe()?, &mut hash)?;
    hash.update(runtime.cache_identity()?.as_bytes());
    for path in files {
        if !path.is_file() {
            continue;
        }
        let name = path
            .file_name()
            .context("model filename missing")?
            .to_string_lossy();
        hash.update((name.len() as u64).to_le_bytes());
        hash.update(name.as_bytes());
        hash.update(fs::metadata(&path)?.len().to_le_bytes());
        hash_file(&path, &mut hash)?;
    }
    Ok(format!("{:x}", hash.finalize()))
}

#[derive(Serialize, Deserialize)]
struct CachedPage {
    identity: String,
    page: crate::mokuro::Page,
}

fn write_json(path: &Path, value: &impl Serialize) -> Result<()> {
    let mut file =
        tempfile::NamedTempFile::new_in(path.parent().context("output parent missing")?)?;
    serde_json::to_writer(file.as_file_mut(), value)?;
    file.flush()?;
    file.as_file_mut().sync_all()?;
    file.persist(path)?;
    Ok(())
}

/// Run native inference on the prepared lossless crops. Cache identities include models and image bytes.
pub fn run(crops: &Path, models: Option<&Path>) -> Result<PathBuf> {
    run_with_runtime(crops, models, &crate::inference::ort::OrtRuntime)
}

/// Run native OCR with a supplied graph runtime; ORT remains the CLI default.
pub fn run_with_runtime(
    crops: &Path,
    models: Option<&Path>,
    runtime: &dyn crate::inference::Runtime,
) -> Result<PathBuf> {
    use crate::{
        mokuro::{Block, Page, Volume},
        native_detector::Detector,
        native_recognizer::Recognizer,
    };
    let models = model_directory(models)?;
    let fingerprint = model_fingerprint(&models, runtime)?;
    let cache = crops
        .parent()
        .context("crop parent missing")?
        .join("native-ocr");
    fs::create_dir_all(&cache)?;
    let mut paths = fs::read_dir(crops)?
        .map(|entry| entry.map(|e| e.path()))
        .collect::<std::io::Result<Vec<_>>>()?;
    paths.retain(|path| path.extension().is_some_and(|ext| ext == "png"));
    paths.sort();
    ensure!(!paths.is_empty(), "no prepared PNG crops found");
    let mut detector = None;
    let mut recognizer = None;
    let mut volume = Volume {
        version: "0.2.5".into(),
        pages: Vec::with_capacity(paths.len()),
    };
    for (ordinal, path) in paths.iter().enumerate() {
        let name = path
            .file_name()
            .context("crop filename missing")?
            .to_str()
            .context("crop filename is not UTF-8")?;
        let identity = cache_identity(path, &fingerprint)?;
        let cached_path = cache.join(format!("{name}.json"));
        if let Ok(bytes) = fs::read(&cached_path)
            && let Ok(cached) = serde_json::from_slice::<CachedPage>(&bytes)
            && cached.identity == identity
            && cached.page.img_path == name
        {
            eprintln!(
                "Native OCR {}/{}: {name} (cached)",
                ordinal + 1,
                paths.len()
            );
            volume.pages.push(cached.page);
            continue;
        }
        if detector.is_none() {
            detector = Some(Detector::load_with_runtime(&models, runtime)?);
            recognizer = Some(Recognizer::load_with_runtime(&models, runtime)?);
        }
        eprintln!("Native OCR {}/{}: {name}", ordinal + 1, paths.len());
        let image = image::open(path)?.to_rgb8();
        let detected = detector
            .as_mut()
            .context("detector missing")?
            .detect(&image)?;
        let mut page = Page {
            img_path: name.into(),
            img_width: image.width(),
            img_height: image.height(),
            blocks: Vec::with_capacity(detected.len()),
        };
        for region in detected {
            let mut block = Block {
                bounds: region.bounds,
                vertical: region.vertical,
                font_size: region.font_size,
                lines: Vec::with_capacity(region.lines.len()),
                lines_coords: Vec::with_capacity(region.lines.len()),
            };
            for line in region.lines {
                let mut text = String::new();
                for crop in line.crops {
                    text.push_str(
                        &recognizer
                            .as_mut()
                            .context("recognizer missing")?
                            .recognize(&crop)?,
                    );
                }
                block.lines.push(text);
                block.lines_coords.push(line.coords);
            }
            page.blocks.push(block);
        }
        let cached = CachedPage { identity, page };
        write_json(&cached_path, &cached)?;
        volume.pages.push(cached.page);
    }
    let output = crops.with_extension("native.mokuro");
    write_json(&output, &volume)?;
    write_json(
        &cache.join("provenance.json"),
        &serde_json::json!({"backend":"native","models_sha256":fingerprint,"pages":volume.pages.len(),"algorithm":"crossink-native-ocr-v1"}),
    )?;
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;

    struct TestRuntime(&'static str);

    impl crate::inference::Runtime for TestRuntime {
        fn cache_identity(&self) -> Result<String> {
            Ok(self.0.into())
        }

        fn load(&self, _: &Path) -> Result<Box<dyn crate::inference::Session>> {
            anyhow::bail!("fingerprinting must not load model sessions")
        }
    }

    #[test]
    fn model_fingerprint_invalidates_ocr_when_runtime_changes() {
        let models = tempfile::tempdir().unwrap();
        fs::write(models.path().join("model.onnx"), b"same model").unwrap();
        let first = model_fingerprint(models.path(), &TestRuntime("runtime-a")).unwrap();
        assert_eq!(
            first,
            model_fingerprint(models.path(), &TestRuntime("runtime-a")).unwrap()
        );
        assert_ne!(
            first,
            model_fingerprint(models.path(), &TestRuntime("runtime-b")).unwrap()
        );
    }

    #[test]
    fn missing_models_fail_without_starting_python() {
        let dir = tempfile::tempdir().unwrap();
        let error = model_directory(Some(&dir.path().join("missing"))).unwrap_err();
        assert!(error.to_string().contains("model directory"));
    }

    #[test]
    fn cache_identity_changes_when_image_or_model_changes() {
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("crop.png");
        std::fs::write(&file, b"original").unwrap();
        let first = cache_identity(&file, "models-a").unwrap();
        assert_ne!(first, cache_identity(&file, "models-b").unwrap());
        std::fs::write(&file, b"modified").unwrap();
        assert_ne!(first, cache_identity(&file, "models-a").unwrap());
    }
}
