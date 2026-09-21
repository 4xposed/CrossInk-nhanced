use std::{fs, path::Path, process::Command};
use tempfile::TempDir;

fn write(root: &Path, name: &str, content: &str) {
    let path = root.join(name);
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    fs::write(path, content).unwrap();
}

#[test]
fn packages_models_runtime_licenses_and_buildable_source() {
    let temp = TempDir::new().unwrap();
    let root = temp.path();
    for name in [
        "LICENSE",
        "LICENSE-MIT",
        "README.md",
        "UPSTREAM-NOTICES.md",
        "VALIDATION.md",
        "third-party-licenses/NOTICE",
        "vendor/example/LICENSE",
        "vendor-config.txt",
        "src/main.rs",
    ] {
        write(root, name, "fixture");
    }
    for name in [
        "comictextdetector.onnx",
        "encoder.onnx",
        "decoder_init.onnx",
        "decoder.onnx",
        "vocab.txt",
        "recognizer.json",
        "provenance.json",
        "detector-LICENSE",
        "recognizer-LICENSE",
    ] {
        write(root, &format!("models-build/{name}"), "model");
    }
    use sha2::{Digest, Sha256};
    let mut exports = serde_json::Map::new();
    for name in [
        "comictextdetector.onnx",
        "encoder.onnx",
        "decoder_init.onnx",
        "decoder.onnx",
        "vocab.txt",
        "recognizer.json",
    ] {
        exports.insert(
            name.into(),
            format!("{:x}", Sha256::digest(b"model")).into(),
        );
    }
    write(
        root,
        "models-build/provenance.json",
        &serde_json::json!({"exports": exports}).to_string(),
    );
    write(root, "models-exported/encoder.onnx", "excluded model");
    write(root, "ort-runtime/onnxruntime/library", "excluded runtime");
    let suffix = std::env::consts::EXE_SUFFIX;
    write(
        root,
        &format!("target/release/crossink-manga{suffix}"),
        "exe",
    );
    write(
        root,
        &format!("target/release/crossink-rar{suffix}"),
        "helper",
    );
    write(root, "runtime/libonnxruntime.so.1.29.0", "runtime");
    write(
        root,
        "runtime/libonnxruntime_providers_shared.so",
        "provider",
    );
    write(root, "runtime/LICENSE", "runtime license");
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(
            root.join("target/release/crossink-manga"),
            fs::Permissions::from_mode(0o755),
        )
        .unwrap();
    }
    let result = Command::new(env!("CARGO_BIN_EXE_crossink-manga-xtask"))
        .args(["package", "--root"])
        .arg(root)
        .args(["--platform", "test", "--models"])
        .arg(root.join("models-build"))
        .arg("--runtime")
        .arg(root.join("runtime/libonnxruntime.so.1.29.0"))
        .arg("--runtime-licenses")
        .arg(root.join("runtime"))
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let mut zip =
        zip::ZipArchive::new(fs::File::open(root.join("dist/crossink-manga-test.zip")).unwrap())
            .unwrap();
    #[cfg(unix)]
    assert_eq!(
        zip.by_name("crossink-manga-test/crossink-manga")
            .unwrap()
            .unix_mode()
            .unwrap()
            & 0o777,
        0o755
    );
    for path in [
        "models/encoder.onnx",
        "licenses/example/LICENSE",
        "licenses/onnxruntime-LICENSE",
        "source/crossink-manga/src/main.rs",
        "source/crossink-manga/.cargo/config.toml",
        "libonnxruntime_providers_shared.so",
    ] {
        assert!(
            zip.by_name(&format!("crossink-manga-test/{path}")).is_ok(),
            "missing {path}"
        );
    }
    assert!(
        !zip.file_names()
            .any(|n| n.contains("/source/crossink-manga/models-build/")
                || n.contains("/source/crossink-manga/models-exported/")
                || n.contains("/source/crossink-manga/ort-runtime/")
                || n.contains("/source/crossink-manga/target/")
                || n.contains("/source/crossink-manga/dist/"))
    );
}

#[test]
fn runtime_discovers_extracted_wheel_without_importing_python() {
    let temp = TempDir::new().unwrap();
    write(
        temp.path(),
        "onnxruntime/capi/libonnxruntime.so.1.29.0",
        "runtime",
    );
    write(temp.path(), "onnxruntime/LICENSE", "license");
    let result = Command::new(env!("CARGO_BIN_EXE_crossink-manga-xtask"))
        .args(["runtime", "--root"])
        .arg(temp.path().join("onnxruntime"))
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let value: serde_json::Value = serde_json::from_slice(&result.stdout).unwrap();
    assert!(
        value["ORT_DYLIB_PATH"]
            .as_str()
            .unwrap()
            .ends_with("libonnxruntime.so.1.29.0")
    );
}

#[test]
fn assets_verify_export_hashes_before_copying() {
    use sha2::{Digest, Sha256};
    let temp = TempDir::new().unwrap();
    let input = temp.path().join("input");
    let output = temp.path().join("output");
    let names = [
        "comictextdetector.onnx",
        "encoder.onnx",
        "decoder_init.onnx",
        "decoder.onnx",
        "vocab.txt",
        "recognizer.json",
    ];
    let mut exports = serde_json::Map::new();
    for name in names {
        write(&input, name, "model");
        exports.insert(
            name.into(),
            format!("{:x}", Sha256::digest(b"model")).into(),
        );
    }
    for name in ["detector-LICENSE", "recognizer-LICENSE"] {
        write(&input, name, "license");
    }
    write(
        &input,
        "provenance.json",
        &serde_json::json!({"exports": exports}).to_string(),
    );
    let run = || {
        Command::new(env!("CARGO_BIN_EXE_crossink-manga-xtask"))
            .args(["assets", "--source"])
            .arg(&input)
            .arg("--output")
            .arg(&output)
            .output()
            .unwrap()
    };
    let result = run();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    assert_eq!(fs::read(output.join("encoder.onnx")).unwrap(), b"model");
    fs::remove_dir_all(&output).unwrap();
    write(&input, "encoder.onnx", "tampered");
    let result = run();
    assert!(!result.status.success());
    assert!(String::from_utf8_lossy(&result.stderr).contains("hash mismatch"));
    assert!(!output.exists(), "verification must precede copying");
}

#[test]
fn runtime_rejects_ambiguous_native_libraries() {
    let temp = TempDir::new().unwrap();
    write(temp.path(), "LICENSE", "license");
    write(temp.path(), "lib/libonnxruntime.so.1", "one");
    write(temp.path(), "lib/libonnxruntime.so.2", "two");
    let result = Command::new(env!("CARGO_BIN_EXE_crossink-manga-xtask"))
        .args(["runtime", "--root"])
        .arg(temp.path())
        .output()
        .unwrap();
    assert!(!result.status.success());
    assert!(String::from_utf8_lossy(&result.stderr).contains("expected one runtime library"));
}

#[test]
fn packaging_rejects_missing_inputs_before_creating_output() {
    let temp = TempDir::new().unwrap();
    let result = Command::new(env!("CARGO_BIN_EXE_crossink-manga-xtask"))
        .args(["package", "--root"])
        .arg(temp.path())
        .args([
            "--platform",
            "test",
            "--models",
            "missing",
            "--runtime",
            "missing",
            "--runtime-licenses",
            "missing",
        ])
        .output()
        .unwrap();
    assert!(!result.status.success());
    assert!(String::from_utf8_lossy(&result.stderr).contains("missing native bundle input"));
    assert!(!temp.path().join("dist").exists());
}

#[test]
fn assets_reject_output_inside_source_before_recursing() {
    let temp = TempDir::new().unwrap();
    let result = Command::new(env!("CARGO_BIN_EXE_crossink-manga-xtask"))
        .args(["assets", "--source"])
        .arg(temp.path())
        .arg("--output")
        .arg(temp.path().join("nested"))
        .output()
        .unwrap();
    assert!(!result.status.success());
    assert!(String::from_utf8_lossy(&result.stderr).contains("outside the source"));
}
