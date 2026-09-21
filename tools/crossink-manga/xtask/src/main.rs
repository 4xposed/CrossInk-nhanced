use anyhow::{Context, Result, ensure};
use clap::{Parser, Subcommand};
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
};
use zip::{ZipWriter, write::SimpleFileOptions};

mod models;

const MODELS: &[&str] = &[
    "comictextdetector.onnx",
    "encoder.onnx",
    "decoder_init.onnx",
    "decoder.onnx",
    "vocab.txt",
    "recognizer.json",
    "provenance.json",
    "detector-LICENSE",
    "recognizer-LICENSE",
];

#[derive(Parser)]
#[command(about = "Assemble native CrossInk manga releases without Python")]
struct Cli {
    #[command(subcommand)]
    command: Task,
}

#[derive(Subcommand)]
enum Task {
    PinModels {
        #[arg(long)]
        source: PathBuf,
        #[arg(long)]
        url: String,
        #[arg(long)]
        archive: PathBuf,
        #[arg(long)]
        lock: PathBuf,
    },
    FetchModels {
        #[arg(long, default_value = "models.lock.json")]
        lock: PathBuf,
        #[arg(long)]
        output: PathBuf,
        #[arg(long)]
        archive: Option<PathBuf>,
    },
    Assets {
        #[arg(long)]
        source: PathBuf,
        #[arg(long)]
        output: PathBuf,
    },
    Runtime {
        #[arg(long)]
        root: PathBuf,
    },
    Package {
        #[arg(long, default_value = ".")]
        root: PathBuf,
        #[arg(long, env = "CARGO_TARGET_DIR")]
        target_dir: Option<PathBuf>,
        #[arg(long, env = "ARTIFACT_PLATFORM")]
        platform: String,
        #[arg(long, env = "NATIVE_MODELS_DIR")]
        models: PathBuf,
        #[arg(long, env = "ORT_DYLIB_PATH")]
        runtime: PathBuf,
        #[arg(long, env = "ORT_LICENSE_DIR")]
        runtime_licenses: PathBuf,
    },
}

fn entries(path: &Path) -> Result<Vec<PathBuf>> {
    let mut paths = fs::read_dir(path)
        .with_context(|| format!("read {}", path.display()))?
        .map(|entry| entry.map(|e| e.path()))
        .collect::<std::io::Result<Vec<_>>>()?;
    paths.sort();
    Ok(paths)
}

fn copy(source: &Path, destination: &Path) -> Result<()> {
    if let Some(parent) = destination.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::copy(source, destination)
        .with_context(|| format!("copy {} to {}", source.display(), destination.display()))?;
    Ok(())
}

fn excluded(path: &Path) -> bool {
    let name = path.file_name().unwrap_or_default().to_string_lossy();
    matches!(
        name.as_ref(),
        "target"
            | "dist"
            | "models-build"
            | "models-exported"
            | "ort-runtime"
            | "__pycache__"
            | ":memory:.ses"
            | ".git"
    ) || name.ends_with(".pyc")
}

fn copy_tree(source: &Path, destination: &Path, source_tree: bool) -> Result<()> {
    fs::create_dir_all(destination)?;
    for path in entries(source)? {
        if source_tree && excluded(&path) {
            continue;
        }
        let target = destination.join(path.file_name().context("missing filename")?);
        // Refuse links rather than accidentally ship files outside the source tree.
        ensure!(
            !fs::symlink_metadata(&path)?.file_type().is_symlink(),
            "symlink in bundle source: {}",
            path.display()
        );
        if path.is_dir() {
            copy_tree(&path, &target, source_tree)?;
        } else {
            copy(&path, &target)?;
        }
    }
    Ok(())
}

fn copy_notices(source: &Path, destination: &Path) -> Result<()> {
    for path in entries(source)? {
        let name = path.file_name().context("missing filename")?;
        let lower = name.to_string_lossy().to_lowercase();
        ensure!(
            !fs::symlink_metadata(&path)?.file_type().is_symlink(),
            "symlink in vendor notices: {}",
            path.display()
        );
        if path.is_dir() {
            copy_notices(&path, &destination.join(name))?;
        } else if ["license", "copying", "notice"]
            .iter()
            .any(|prefix| lower.starts_with(prefix))
        {
            copy(&path, &destination.join(name))?;
        }
    }
    Ok(())
}

fn zip_tree(writer: &mut ZipWriter<fs::File>, root: &Path, directory: &Path) -> Result<()> {
    for path in entries(directory)? {
        if path.is_dir() {
            zip_tree(writer, root, &path)?;
            continue;
        }
        let name = path
            .strip_prefix(root)?
            .to_string_lossy()
            .replace('\\', "/");
        let options =
            SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
        #[cfg(unix)]
        let options = {
            use std::os::unix::fs::PermissionsExt;
            options.unix_permissions(fs::metadata(&path)?.permissions().mode())
        };
        writer.start_file(name, options)?;
        std::io::copy(&mut fs::File::open(&path)?, writer)?;
    }
    Ok(())
}

fn emit_environment(values: serde_json::Value) -> Result<()> {
    println!("{}", serde_json::to_string_pretty(&values)?);
    if let Some(path) = std::env::var_os("GITHUB_ENV") {
        let mut file = fs::OpenOptions::new()
            .append(true)
            .create(true)
            .open(path)?;
        for (key, value) in values.as_object().context("expected environment object")? {
            let value = value
                .as_str()
                .context("expected string environment value")?;
            ensure!(
                !value.contains(['\n', '\r']),
                "environment path contains newline"
            );
            writeln!(file, "{key}={value}")?;
        }
    }
    Ok(())
}

fn runtime(root: &Path) -> Result<()> {
    let root = root.canonicalize()?;
    ensure!(
        root.join("LICENSE").is_file(),
        "missing runtime LICENSE in {}",
        root.display()
    );
    let directory = if root.join("capi").is_dir() {
        root.join("capi")
    } else if root.join("lib").is_dir() {
        root.join("lib")
    } else {
        root.clone()
    };
    let binaries: Vec<_> = entries(&directory)?
        .into_iter()
        .filter(|path| {
            let name = path.file_name().unwrap_or_default().to_string_lossy();
            path.is_file() && (name.starts_with("libonnxruntime.") || name == "onnxruntime.dll")
        })
        .collect();
    ensure!(
        binaries.len() == 1,
        "expected one runtime library in {}, found {}",
        directory.display(),
        binaries.len()
    );
    emit_environment(serde_json::json!({"ORT_DYLIB_PATH": binaries[0], "ORT_LICENSE_DIR": root}))
}

fn package(
    root: &Path,
    target_dir: Option<&Path>,
    platform: &str,
    models: &Path,
    runtime: &Path,
    licenses: &Path,
) -> Result<PathBuf> {
    ensure!(
        !platform.is_empty()
            && platform
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_'),
        "platform must contain only letters, digits, hyphens and underscores"
    );
    let target = target_dir
        .map(Path::to_path_buf)
        .unwrap_or_else(|| root.join("target"));
    let binary = target
        .join("release")
        .join(format!("crossink-manga{}", std::env::consts::EXE_SUFFIX));
    let helper = target
        .join("release")
        .join(format!("crossink-rar{}", std::env::consts::EXE_SUFFIX));
    for path in [
        binary.clone(),
        helper.clone(),
        runtime.to_path_buf(),
        licenses.join("LICENSE"),
    ]
    .into_iter()
    .chain(MODELS.iter().map(|name| models.join(name)))
    {
        ensure!(
            path.is_file(),
            "missing native bundle input: {}",
            path.display()
        );
    }
    verify_assets(models)?;
    let runtime_name = if runtime.file_name().is_some_and(|n| n == "onnxruntime.dll") {
        "onnxruntime.dll"
    } else if runtime.extension().is_some_and(|n| n == "dylib") {
        "libonnxruntime.dylib"
    } else {
        "libonnxruntime.so"
    };
    let dist = root.join("dist");
    fs::create_dir_all(&dist)?;
    let bundle = dist.join(format!("crossink-manga-{platform}"));
    let archive = dist.join(format!("crossink-manga-{platform}.zip"));
    ensure!(
        !bundle.exists() && !archive.exists(),
        "bundle output already exists: {}",
        bundle.display()
    );
    fs::create_dir(&bundle)?;
    for path in [&binary, &helper] {
        copy(
            path,
            &bundle.join(path.file_name().context("missing filename")?),
        )?;
    }
    copy(runtime, &bundle.join(runtime_name))?;
    for path in entries(runtime.parent().context("runtime has no parent")?)? {
        if path.is_file()
            && path
                .file_name()
                .unwrap_or_default()
                .to_string_lossy()
                .contains("onnxruntime_providers")
        {
            copy(
                &path,
                &bundle.join(path.file_name().context("missing filename")?),
            )?;
        }
    }
    copy_tree(models, &bundle.join("models"), false)?;
    for name in [
        "LICENSE-MIT",
        "UPSTREAM-NOTICES.md",
        "README.md",
        "VALIDATION.md",
        "LICENSE",
    ] {
        copy(&root.join(name), &bundle.join(name))?;
    }
    let notices = bundle.join("licenses");
    copy_tree(&root.join("third-party-licenses"), &notices, false)?;
    copy_notices(&root.join("vendor"), &notices)?;
    for path in entries(licenses)? {
        let name = path
            .file_name()
            .context("missing filename")?
            .to_string_lossy();
        let lower = name.to_lowercase();
        if path.is_file() && (lower.contains("license") || lower.contains("notice")) {
            copy(&path, &notices.join(format!("onnxruntime-{name}")))?;
        }
    }
    let source = bundle.join("source/crossink-manga");
    copy_tree(root, &source, true)?;
    copy(
        &root.join("vendor-config.txt"),
        &source.join(".cargo/config.toml"),
    )?;
    let mut writer = ZipWriter::new(
        fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&archive)?,
    );
    zip_tree(&mut writer, &dist, &bundle)?;
    writer.finish()?;
    Ok(archive)
}

fn main() -> Result<()> {
    match Cli::parse().command {
        Task::PinModels {
            source,
            url,
            archive,
            lock,
        } => models::pin(&source, &url, &archive, &lock),
        Task::FetchModels {
            lock,
            output,
            archive,
        } => {
            models::fetch(&lock, &output, archive.as_deref())?;
            emit_environment(serde_json::json!({"NATIVE_MODELS_DIR": output.canonicalize()?}))
        }
        Task::Assets { source, output } => {
            let source = source.canonicalize()?;
            let output = std::path::absolute(output)?;
            let parent = output
                .parent()
                .context("asset output has no parent")?
                .canonicalize()?;
            ensure!(
                !parent.starts_with(&source),
                "asset output must be outside the source directory"
            );
            verify_assets(&source)?;
            ensure!(
                !output.exists(),
                "asset output already exists: {}",
                output.display()
            );
            copy_tree(&source, &output, false)?;
            emit_environment(serde_json::json!({"NATIVE_MODELS_DIR": output.canonicalize()?}))
        }
        Task::Runtime { root } => runtime(&root),
        Task::Package {
            root,
            target_dir,
            platform,
            models,
            runtime,
            runtime_licenses,
        } => {
            println!(
                "{}",
                package(
                    &root,
                    target_dir.as_deref(),
                    &platform,
                    &models,
                    &runtime,
                    &runtime_licenses
                )?
                .display()
            );
            Ok(())
        }
    }
}

fn verify_assets(source: &Path) -> Result<()> {
    for name in MODELS {
        ensure!(source.join(name).is_file(), "missing model input: {name}");
    }
    let provenance: serde_json::Value =
        serde_json::from_reader(fs::File::open(source.join("provenance.json"))?)?;
    for name in &MODELS[..6] {
        let expected = provenance["exports"][name]
            .as_str()
            .with_context(|| format!("missing provenance digest for {name}"))?;
        let mut file = fs::File::open(source.join(name))?;
        let mut digest = Sha256::new();
        std::io::copy(&mut file, &mut digest)?;
        ensure!(
            format!("{:x}", digest.finalize()) == expected,
            "model hash mismatch: {name}"
        );
    }
    Ok(())
}
