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

fn v2_fixture(root: &Path) -> serde_json::Value {
    fixture(&root.join("source"));
    fs::remove_file(root.join("source/decoder_init.onnx")).unwrap();
    let provenance_path = root.join("source/provenance.json");
    let mut provenance: serde_json::Value =
        serde_json::from_slice(&fs::read(&provenance_path).unwrap()).unwrap();
    fs::write(root.join("source/detector.json"), b"{}").unwrap();
    provenance["exports"]["detector.json"] = format!("{:x}", Sha256::digest(b"{}")).into();
    provenance["model_interface"] = "crossink-native-ocr-v2".into();
    provenance["exports"]
        .as_object_mut()
        .unwrap()
        .remove("decoder_init.onnx");
    fs::write(provenance_path, provenance.to_string()).unwrap();
    let mut files = serde_json::Map::new();
    for entry in fs::read_dir(root.join("source")).unwrap() {
        let entry = entry.unwrap();
        let name = entry.file_name().to_str().unwrap().to_owned();
        let bytes = fs::read(entry.path()).unwrap();
        files.insert(name.clone(), serde_json::json!({"bytes": bytes.len(), "sha256":format!("{:x}",Sha256::digest(&bytes)), "local":format!("source/{name}")}));
    }
    serde_json::json!({"schema_version":2,"model_interface":"crossink-native-ocr-v2","files":files})
}

fn fetch_v2(root: &Path, lock: &serde_json::Value) -> Output {
    fs::write(root.join("models.lock.json"), lock.to_string()).unwrap();
    Command::new(env!("CARGO_BIN_EXE_crossink-manga-xtask"))
        .args(["fetch-models", "--lock"])
        .arg(root.join("models.lock.json"))
        .arg("--output")
        .arg(root.join("output"))
        .output()
        .unwrap()
}

#[test]
fn v2_fetches_verified_local_metadata_without_decoder_init() {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path();
    let lock = v2_fixture(root);
    let result = fetch_v2(root, &lock);
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    assert_eq!(
        fs::read(root.join("output/encoder.onnx")).unwrap(),
        b"encoder.onnx"
    );
    assert!(!root.join("output/decoder_init.onnx").exists());
}

#[test]
fn v2_rejects_unsafe_or_ambiguous_local_sources_and_bad_hashes() {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path();
    let lock = v2_fixture(root);
    for unsafe_path in [
        "../source/encoder.onnx",
        "/etc/passwd",
        "source/../source/encoder.onnx",
    ] {
        let mut invalid = lock.clone();
        invalid["files"]["encoder.onnx"]["local"] = unsafe_path.into();
        assert!(!fetch_v2(root, &invalid).status.success());
        assert!(!root.join("output").exists());
    }
    let mut invalid = lock.clone();
    invalid["files"]["encoder.onnx"]["url"] = "https://example.invalid/model".into();
    assert!(!fetch_v2(root, &invalid).status.success());
    let mut invalid = lock.clone();
    invalid["files"]["encoder.onnx"]["sha256"] = "0".repeat(64).into();
    assert!(!fetch_v2(root, &invalid).status.success());
    assert!(!root.join("output").exists());
    #[cfg(unix)]
    {
        fs::rename(root.join("source/encoder.onnx"), root.join("real-encoder")).unwrap();
        std::os::unix::fs::symlink(root.join("real-encoder"), root.join("source/encoder.onnx"))
            .unwrap();
        assert!(!fetch_v2(root, &lock).status.success());
        assert!(!root.join("output").exists());
    }
}

#[test]
fn v2_refuses_archive_override_and_existing_output() {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path();
    let lock = v2_fixture(root);
    fs::write(root.join("models.lock.json"), lock.to_string()).unwrap();
    let result = fetch(root);
    assert!(!result.status.success());
    assert!(String::from_utf8_lossy(&result.stderr).contains("--archive"));
    fs::create_dir(root.join("output")).unwrap();
    fs::write(root.join("output/user-file"), b"keep").unwrap();
    assert!(!fetch_v2(root, &lock).status.success());
    assert_eq!(fs::read(root.join("output/user-file")).unwrap(), b"keep");
}

#[cfg(unix)]
#[test]
fn v2_downloads_https_file_and_checks_downloaded_bytes() {
    use std::os::unix::fs::PermissionsExt;
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path();
    let mut lock = v2_fixture(root);
    lock["files"]["encoder.onnx"]
        .as_object_mut()
        .unwrap()
        .remove("local");
    lock["files"]["encoder.onnx"]["url"] = "https://example.invalid/encoder.onnx".into();
    let bin = root.join("bin");
    fs::create_dir(&bin).unwrap();
    let curl = bin.join("curl");
    fs::write(&curl, "#!/bin/sh\nwhile [ \"$#\" -gt 0 ]; do\nif [ \"$1\" = --output ]; then shift; target=$1; fi\nshift\ndone\nprintf encoder.onnx > \"$target\"\n").unwrap();
    fs::set_permissions(&curl, fs::Permissions::from_mode(0o755)).unwrap();
    let run = |lock: &serde_json::Value| {
        fs::write(root.join("models.lock.json"), lock.to_string()).unwrap();
        Command::new(env!("CARGO_BIN_EXE_crossink-manga-xtask"))
            .args(["fetch-models", "--lock"])
            .arg(root.join("models.lock.json"))
            .arg("--output")
            .arg(root.join("output"))
            .env("PATH", &bin)
            .output()
            .unwrap()
    };
    let result = run(&lock);
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    assert_eq!(
        fs::read(root.join("output/encoder.onnx")).unwrap(),
        b"encoder.onnx"
    );
    fs::remove_dir_all(root.join("output")).unwrap();
    lock["files"]["encoder.onnx"]["sha256"] = "0".repeat(64).into();
    let result = run(&lock);
    assert!(!result.status.success());
    assert!(String::from_utf8_lossy(&result.stderr).contains("SHA-256 mismatch"));
    assert!(!root.join("output").exists());
}
