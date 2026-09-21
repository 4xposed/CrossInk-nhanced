use sha2::{Digest, Sha256};
use std::{
    fs,
    path::Path,
    process::{Command, Output},
};

fn run(args: &[&str], paths: &[&Path]) -> Output {
    let mut command = Command::new(env!("CARGO_BIN_EXE_crossink-manga-xtask"));
    command.args(args);
    for path in paths {
        command.arg(path);
    }
    command.output().unwrap()
}

fn fixture(root: &Path) {
    fs::create_dir(root).unwrap();
    let mut exports = serde_json::Map::new();
    for name in [
        "comictextdetector.onnx",
        "encoder.onnx",
        "decoder_init.onnx",
        "decoder.onnx",
        "vocab.txt",
        "recognizer.json",
    ] {
        fs::write(root.join(name), name).unwrap();
        exports.insert(
            name.into(),
            format!("{:x}", Sha256::digest(name.as_bytes())).into(),
        );
    }
    fs::write(
        root.join("provenance.json"),
        serde_json::json!({"exports":exports}).to_string(),
    )
    .unwrap();
    for name in ["detector-LICENSE", "recognizer-LICENSE", "model-README.md"] {
        fs::write(root.join(name), name).unwrap();
    }
}

fn pin(root: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_crossink-manga-xtask"))
        .args(["pin-models", "--source"])
        .arg(root.join("source"))
        .args([
            "--url",
            "https://example.invalid/models-v1.zip",
            "--archive",
        ])
        .arg(root.join("bundle.zip"))
        .arg("--lock")
        .arg(root.join("models.lock.json"))
        .output()
        .unwrap()
}

fn fetch(root: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_crossink-manga-xtask"))
        .args(["fetch-models", "--lock"])
        .arg(root.join("models.lock.json"))
        .arg("--archive")
        .arg(root.join("bundle.zip"))
        .arg("--output")
        .arg(root.join("output"))
        .output()
        .unwrap()
}

#[test]
fn pin_and_fetch_preserve_models_provenance_and_licenses() {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path();
    fixture(&root.join("source"));
    let result = pin(root);
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let lock: serde_json::Value =
        serde_json::from_slice(&fs::read(root.join("models.lock.json")).unwrap()).unwrap();
    assert_eq!(lock["schema_version"], 1);
    assert_eq!(
        lock["files"]["detector-LICENSE"]["sha256"],
        format!("{:x}", Sha256::digest(b"detector-LICENSE"))
    );
    let result = fetch(root);
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    for entry in fs::read_dir(root.join("source")).unwrap() {
        let entry = entry.unwrap();
        assert_eq!(
            fs::read(entry.path()).unwrap(),
            fs::read(root.join("output").join(entry.file_name())).unwrap()
        );
    }
}

#[test]
fn source_metadata_subdirectories_are_pinned_and_verified() {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path();
    fixture(&root.join("source"));
    fs::create_dir(root.join("source/source-metadata")).unwrap();
    fs::write(
        root.join("source/source-metadata/config.json"),
        b"source model config",
    )
    .unwrap();
    let result = pin(root);
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    assert!(fetch(root).status.success());
    assert_eq!(
        fs::read(root.join("output/source-metadata/config.json")).unwrap(),
        b"source model config"
    );
}

#[test]
fn tampered_archive_is_rejected_without_publishing_models() {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path();
    fixture(&root.join("source"));
    assert!(pin(root).status.success());
    fs::write(root.join("bundle.zip"), b"tampered").unwrap();
    let result = fetch(root);
    assert!(!result.status.success());
    assert!(!root.join("output").exists());
}

#[test]
fn wrong_individual_license_hash_is_rejected() {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path();
    fixture(&root.join("source"));
    assert!(pin(root).status.success());
    let path = root.join("models.lock.json");
    let mut lock: serde_json::Value = serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
    lock["files"]["recognizer-LICENSE"]["sha256"] = "0".repeat(64).into();
    fs::write(&path, lock.to_string()).unwrap();
    let result = fetch(root);
    assert!(!result.status.success());
    assert!(!root.join("output").exists());
}

#[test]
fn no_existing_artifact_is_overwritten() {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path();
    fixture(&root.join("source"));
    fs::write(root.join("models.lock.json"), b"existing").unwrap();
    assert!(!pin(root).status.success());
    assert_eq!(
        fs::read(root.join("models.lock.json")).unwrap(),
        b"existing"
    );
    assert!(!root.join("bundle.zip").exists());
}

#[test]
fn pin_requires_an_https_artifact_location() {
    let result = run(
        &[
            "pin-models",
            "--source",
            "missing",
            "--url",
            "http://example.invalid/model.zip",
            "--archive",
            "missing.zip",
            "--lock",
        ],
        &[Path::new("missing.json")],
    );
    assert!(!result.status.success());
}

#[test]
fn identical_exports_produce_identical_archives_and_file_hashes() {
    let first = tempfile::tempdir().unwrap();
    let second = tempfile::tempdir().unwrap();
    for root in [first.path(), second.path()] {
        fixture(&root.join("source"));
        assert!(pin(root).status.success());
    }
    assert_eq!(
        fs::read(first.path().join("bundle.zip")).unwrap(),
        fs::read(second.path().join("bundle.zip")).unwrap()
    );
    assert_eq!(
        fs::read(first.path().join("models.lock.json")).unwrap(),
        fs::read(second.path().join("models.lock.json")).unwrap()
    );
}

#[test]
fn pin_does_not_publish_a_bundle_exceeding_fetch_file_limit() {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path();
    fixture(&root.join("source"));
    for index in 0..128 {
        fs::write(root.join(format!("source/notice-{index}.txt")), "notice").unwrap();
    }
    let result = pin(root);
    assert!(!result.status.success());
    assert!(!root.join("models.lock.json").exists());
    assert!(!root.join("bundle.zip").exists());
}

#[test]
fn existing_output_is_preserved_and_source_symlinks_are_refused() {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path();
    fixture(&root.join("source"));
    assert!(pin(root).status.success());
    fs::create_dir(root.join("output")).unwrap();
    fs::write(root.join("output/user-file"), b"keep").unwrap();
    assert!(!fetch(root).status.success());
    assert_eq!(fs::read(root.join("output/user-file")).unwrap(), b"keep");
    #[cfg(unix)]
    {
        fs::remove_file(root.join("bundle.zip")).unwrap();
        fs::remove_file(root.join("models.lock.json")).unwrap();
        std::os::unix::fs::symlink(
            root.join("source/encoder.onnx"),
            root.join("source/link.onnx"),
        )
        .unwrap();
        assert!(!pin(root).status.success());
        assert!(!root.join("bundle.zip").exists());
    }
}

#[test]
fn locked_traversal_filename_is_rejected_before_output_creation() {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path();
    fixture(&root.join("source"));
    assert!(pin(root).status.success());
    let path = root.join("models.lock.json");
    let mut lock: serde_json::Value = serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
    lock["files"]["../escape"] = lock["files"]["encoder.onnx"].clone();
    fs::write(path, lock.to_string()).unwrap();
    let result = fetch(root);
    assert!(!result.status.success());
    assert!(String::from_utf8_lossy(&result.stderr).contains("invalid model filename"));
    assert!(!root.join("output").exists());
}
