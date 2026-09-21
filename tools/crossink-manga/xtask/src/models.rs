//! Content-addressed ONNX bundle creation and retrieval. No exporter is executed.
use anyhow::{Context, Result, ensure};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::{Read, Seek, SeekFrom, Write},
    path::{Component, Path, PathBuf},
    process::Command,
};
use zip::{ZipArchive, ZipWriter, write::SimpleFileOptions};

const INTERFACE: &str = "crossink-native-ocr-v1";
const MAX_BUNDLE_BYTES: u64 = 16 * 1024 * 1024 * 1024;
const MAX_FILES: usize = 128;

fn https(url: &str) -> Result<()> {
    let authority = url
        .strip_prefix("https://")
        .context("model artifact URL must use HTTPS")?
        .split('/')
        .next()
        .unwrap_or("");
    ensure!(
        !authority.is_empty()
            && !authority.contains('@')
            && !url.chars().any(char::is_whitespace)
            && !url.contains(['\\', '\0']),
        "invalid public artifact URL"
    );
    Ok(())
}

fn filename(name: &str) -> Result<()> {
    ensure!(
        !name.is_empty()
            && !name.contains(['\\', ':', '\0'])
            && name
                .split('/')
                .all(|part| !part.is_empty() && part != "." && part != "..")
            && Path::new(name)
                .components()
                .all(|c| matches!(c, Component::Normal(_))),
        "invalid model filename: {name}"
    );
    Ok(())
}

fn source_files(directory: &Path, paths: &mut Vec<PathBuf>) -> Result<()> {
    for path in super::entries(directory)? {
        let kind = fs::symlink_metadata(&path)?.file_type();
        ensure!(
            !kind.is_symlink(),
            "model source contains a symlink: {}",
            path.display()
        );
        if kind.is_dir() {
            source_files(&path, paths)?;
        } else {
            ensure!(
                kind.is_file(),
                "model source must contain regular files only: {}",
                path.display()
            );
            paths.push(path);
            ensure!(paths.len() <= MAX_FILES, "too many model files");
        }
    }
    Ok(())
}

fn relative_name(source: &Path, path: &Path) -> Result<String> {
    let relative = path
        .strip_prefix(source)?
        .to_str()
        .context("non-UTF8 model filename")?;
    let name = relative.replace('\\', "/");
    filename(&name)?;
    Ok(name)
}

fn hash(file: &mut fs::File) -> Result<String> {
    file.seek(SeekFrom::Start(0))?;
    let mut digest = Sha256::new();
    std::io::copy(file, &mut digest)?;
    Ok(format!("{:x}", digest.finalize()))
}

fn descriptor(path: &Path) -> Result<Value> {
    let mut file = fs::File::open(path)?;
    Ok(json!({"sha256":hash(&mut file)?,"bytes":file.metadata()?.len()}))
}

fn size(value: &Value) -> Result<u64> {
    let bytes = value["bytes"]
        .as_u64()
        .context("missing artifact byte size")?;
    ensure!(bytes <= MAX_BUNDLE_BYTES, "artifact exceeds 16 GiB limit");
    Ok(bytes)
}

fn expected_hash(value: &Value) -> Result<&str> {
    let digest = value["sha256"].as_str().context("missing SHA-256 digest")?;
    ensure!(
        digest.len() == 64
            && digest
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)),
        "invalid SHA-256 digest"
    );
    Ok(digest)
}

fn destination(path: &Path) -> Result<(PathBuf, PathBuf)> {
    let absolute = std::path::absolute(path)?;
    ensure!(
        !absolute.try_exists()? && !absolute.is_symlink(),
        "output already exists: {}",
        absolute.display()
    );
    let parent = absolute
        .parent()
        .context("output has no parent")?
        .canonicalize()?;
    let target = parent.join(absolute.file_name().context("output has no filename")?);
    Ok((parent, target))
}

pub fn pin(source: &Path, url: &str, archive: &Path, lock: &Path) -> Result<()> {
    https(url)?;
    let source = source.canonicalize()?;
    let (archive_parent, archive) = destination(archive)?;
    let (lock_parent, lock) = destination(lock)?;
    ensure!(
        archive != lock,
        "archive and lockfile must have different paths"
    );
    ensure!(
        !archive.starts_with(&source) && !lock.starts_with(&source),
        "pin outputs must be outside model source"
    );
    // ZIP pins retain the original v1 graph interface.
    for name in super::MODELS {
        ensure!(
            source.join(name).is_file(),
            "missing v1 model input: {name}"
        );
    }
    super::verify_assets(&source)?;
    let mut files = serde_json::Map::new();
    let mut paths = Vec::new();
    source_files(&source, &mut paths)?;
    ensure!(paths.len() <= MAX_FILES, "too many model files");
    let mut total = 0u64;
    for path in &paths {
        ensure!(
            fs::symlink_metadata(path)?.file_type().is_file(),
            "model source must contain regular files only: {}",
            path.display()
        );
        let name = relative_name(&source, path)?;
        let info = descriptor(path)?;
        total = total
            .checked_add(size(&info)?)
            .context("model size overflow")?;
        ensure!(
            total <= MAX_BUNDLE_BYTES,
            "model bundle exceeds 16 GiB limit"
        );
        files.insert(name, info);
    }
    let mut temporary = tempfile::NamedTempFile::new_in(&archive_parent)?;
    {
        let mut writer = ZipWriter::new(temporary.as_file_mut());
        // ONNX files are already large binary containers. Stored ZIP members
        // avoid compressor-version dependence and make pinning reproducible.
        let options = SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Stored)
            .last_modified_time(zip::DateTime::default())
            .unix_permissions(0o644);
        for path in paths {
            let relative = relative_name(&source, &path)?;
            let name = relative.as_str();
            writer.start_file(
                name,
                options.large_file(size(&files[name])? >= u32::MAX as u64),
            )?;
            // Hash the actual bytes written, not just an earlier source read.
            // This host-side buffer also avoids loading ONNX graphs into RAM.
            let mut input = fs::File::open(&path)?;
            let mut buffer = vec![0; 64 * 1024];
            let mut digest = Sha256::new();
            let mut copied = 0u64;
            loop {
                let count = input.read(&mut buffer)?;
                if count == 0 {
                    break;
                }
                copied += count as u64;
                ensure!(
                    copied <= size(&files[name])?,
                    "model source changed while pinning: {name}"
                );
                writer.write_all(&buffer[..count])?;
                digest.update(&buffer[..count]);
            }
            ensure!(
                copied == size(&files[name])?
                    && format!("{:x}", digest.finalize()) == expected_hash(&files[name])?,
                "model source changed while pinning: {name}"
            );
        }
        writer.finish()?;
    }
    let mut artifact = descriptor(temporary.path())?;
    size(&artifact)?;
    artifact["url"] = url.into();
    let manifest =
        json!({"schema_version":1,"model_interface":INTERFACE,"archive":artifact,"files":files});
    let mut manifest_file = tempfile::NamedTempFile::new_in(lock_parent)?;
    serde_json::to_writer_pretty(&mut manifest_file, &manifest)?;
    writeln!(manifest_file)?;
    temporary
        .persist_noclobber(&archive)
        .context("publish model archive")?;
    manifest_file
        .persist_noclobber(&lock)
        .context("publish model lockfile")?;
    println!("Pinned {} in {}", archive.display(), lock.display());
    Ok(())
}

pub fn fetch(lock: &Path, output: &Path, archive: Option<&Path>) -> Result<()> {
    let manifest_file = fs::File::open(lock).context("open model lockfile")?;
    ensure!(
        manifest_file.metadata()?.len() <= 1024 * 1024,
        "model lockfile exceeds 1 MiB"
    );
    let manifest: Value = serde_json::from_reader(manifest_file)?;
    if manifest["schema_version"] == 2 {
        ensure!(
            archive.is_none(),
            "--archive is unsupported for per-file model pins"
        );
        return fetch_files(lock, output, &manifest);
    }
    ensure!(
        manifest["schema_version"] == 1 && manifest["model_interface"] == INTERFACE,
        "unsupported model lockfile schema or interface"
    );
    let url = manifest["archive"]["url"]
        .as_str()
        .context("missing artifact URL")?;
    https(url)?;
    let archive_size = size(&manifest["archive"])?;
    let archive_hash = expected_hash(&manifest["archive"])?;
    let files = manifest["files"]
        .as_object()
        .context("missing locked file list")?;
    ensure!(files.len() <= MAX_FILES, "too many locked model files");
    for name in super::MODELS {
        ensure!(
            files.contains_key(*name),
            "missing locked model file {name}"
        );
    }
    let mut total = 0u64;
    for (name, info) in files {
        filename(name)?;
        expected_hash(info)?;
        total = total
            .checked_add(size(info)?)
            .context("model size overflow")?;
        ensure!(
            total <= MAX_BUNDLE_BYTES,
            "model bundle exceeds 16 GiB limit"
        );
    }
    let (parent, output) = destination(output)?;
    let download = tempfile::NamedTempFile::new_in(&parent)?;
    let archive = if let Some(path) = archive {
        path
    } else {
        let status = Command::new("curl")
            .args([
                "--fail",
                "--location",
                "--silent",
                "--show-error",
                "--proto",
                "=https",
                "--proto-redir",
                "=https",
                "--retry",
                "3",
                "--max-time",
                "1800",
                "--max-filesize",
                &archive_size.to_string(),
                "--output",
            ])
            .arg(download.path())
            .arg("--")
            .arg(url)
            .status()
            .context("model download requires native curl")?;
        ensure!(status.success(), "model artifact download failed");
        download.path()
    };
    let mut file = fs::File::open(archive)?;
    ensure!(
        file.metadata()?.len() == archive_size,
        "model archive size mismatch"
    );
    ensure!(
        hash(&mut file)? == archive_hash,
        "model archive SHA-256 mismatch"
    );
    file.rewind()?;
    let mut zip = ZipArchive::new(file)?;
    ensure!(
        zip.len() == files.len(),
        "model archive member count mismatch"
    );
    let staging = tempfile::tempdir_in(&parent)?;
    let mut seen = std::collections::BTreeSet::new();
    for index in 0..zip.len() {
        let mut member = zip.by_index(index)?;
        let name = member.name().to_owned();
        filename(&name)?;
        ensure!(
            seen.insert(name.clone()),
            "duplicate model archive member {name}"
        );
        let info = files
            .get(&name)
            .with_context(|| format!("unlisted model file {name}"))?;
        ensure!(
            !member.is_dir() && !member.unix_mode().is_some_and(|m| m & 0o170000 == 0o120000),
            "model archive member must be a regular file"
        );
        let expected_size = size(info)?;
        ensure!(
            member.size() == expected_size,
            "model file size mismatch: {name}"
        );
        let target = staging.path().join(&name);
        fs::create_dir_all(target.parent().context("model output has no parent")?)?;
        let mut output_file = fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&target)?;
        let copied = std::io::copy(&mut (&mut member).take(expected_size + 1), &mut output_file)?;
        ensure!(
            copied == expected_size,
            "expanded model size mismatch: {name}"
        );
        drop(output_file);
        ensure!(
            hash(&mut fs::File::open(target)?)? == expected_hash(info)?,
            "model file SHA-256 mismatch: {name}"
        );
    }
    super::verify_assets(staging.path())?;
    ensure!(
        !output.try_exists()? && !output.is_symlink(),
        "model output already exists"
    );
    publish_directory(staging.path(), &output)?;
    Ok(())
}

// Local metadata is confined to the lockfile directory, including every path
// component. Never follow a link into an unrelated directory or file.
fn local_source(root: &Path, name: &str) -> Result<PathBuf> {
    filename(name)?;
    let mut source = root.to_path_buf();
    for part in name.split('/') {
        source.push(part);
        ensure!(
            !fs::symlink_metadata(&source)?.file_type().is_symlink(),
            "symlink in local model metadata: {}",
            source.display()
        );
    }
    ensure!(
        source.is_file(),
        "local model metadata must be a regular file"
    );
    Ok(source)
}

fn download_file(url: &str, target: &Path, bytes: u64) -> Result<()> {
    https(url)?;
    // curl also applies this limit to redirect responses. A host's redirect
    // body can exceed a small JSON file; still check the final exact size below.
    let transfer_limit = bytes.max(1024 * 1024);
    let status = Command::new("curl")
        .args([
            "--fail",
            "--location",
            "--silent",
            "--show-error",
            "--proto",
            "=https",
            "--proto-redir",
            "=https",
            "--retry",
            "3",
            "--max-time",
            "1800",
            "--max-filesize",
            &transfer_limit.to_string(),
            "--output",
        ])
        .arg(target)
        .arg("--")
        .arg(url)
        .status()
        .context("model download requires native curl")?;
    ensure!(status.success(), "model file download failed: {url}");
    Ok(())
}

fn fetch_files(lock: &Path, output: &Path, manifest: &Value) -> Result<()> {
    ensure!(
        manifest["model_interface"] == "crossink-native-ocr-v2",
        "unsupported per-file model interface"
    );
    let files = manifest["files"]
        .as_object()
        .context("missing locked file list")?;
    ensure!(files.len() <= MAX_FILES, "too many locked model files");
    for name in super::MODELS_V2 {
        ensure!(
            files.contains_key(*name),
            "missing locked model file {name}"
        );
    }
    let lock = lock.canonicalize()?;
    let root = lock.parent().context("model lockfile has no parent")?;
    let mut total = 0u64;
    // Validate the complete manifest before downloading anything.
    for (name, info) in files {
        filename(name)?;
        expected_hash(info)?;
        total = total
            .checked_add(size(info)?)
            .context("model size overflow")?;
        ensure!(
            total <= MAX_BUNDLE_BYTES,
            "model bundle exceeds 16 GiB limit"
        );
        ensure!(
            info.get("url").is_some() != info.get("local").is_some(),
            "model file must specify exactly one url or local source: {name}"
        );
        if let Some(url) = info.get("url") {
            https(url.as_str().context("model URL must be a string")?)?;
        } else {
            local_source(
                root,
                info["local"]
                    .as_str()
                    .context("local source must be a string")?,
            )?;
        }
    }
    let (parent, output) = destination(output)?;
    let staging = tempfile::tempdir_in(&parent)?;
    for (name, info) in files {
        let target = staging.path().join(name);
        fs::create_dir_all(target.parent().context("model file has no parent")?)?;
        if let Some(url) = info["url"].as_str() {
            download_file(url, &target, size(info)?)?;
        } else {
            let source = local_source(
                root,
                info["local"].as_str().context("missing local source")?,
            )?;
            let mut source = fs::File::open(source)?;
            ensure!(
                source.metadata()?.len() == size(info)?,
                "local model size mismatch: {name}"
            );
            let mut file = fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&target)?;
            std::io::copy(&mut (&mut source).take(size(info)? + 1), &mut file)?;
        }
        let mut file = fs::File::open(&target)?;
        ensure!(
            file.metadata()?.len() == size(info)?,
            "model file size mismatch: {name}"
        );
        ensure!(
            hash(&mut file)? == expected_hash(info)?,
            "model file SHA-256 mismatch: {name}"
        );
    }
    let provenance: Value =
        serde_json::from_reader(fs::File::open(staging.path().join("provenance.json"))?)?;
    ensure!(
        provenance["model_interface"] == manifest["model_interface"],
        "model provenance interface does not match lockfile"
    );
    super::verify_assets(staging.path())?;
    publish_directory(staging.path(), &output)?;
    Ok(())
}

fn publish_directory(source: &Path, output: &Path) -> Result<()> {
    #[cfg(any(target_os = "linux", target_vendor = "apple"))]
    {
        use rustix::fs::{CWD, RenameFlags, renameat_with};
        renameat_with(CWD, source, CWD, output, RenameFlags::NOREPLACE)
            .context("publish verified models without replacing existing output")?;
    }
    #[cfg(windows)]
    {
        // Windows directory renames refuse an existing destination directory.
        fs::rename(source, output).context("publish verified model directory")?;
    }
    #[cfg(not(any(target_os = "linux", target_vendor = "apple", windows)))]
    anyhow::bail!("atomic model directory publication is unsupported on this platform");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn publication_refuses_even_an_empty_directory_created_after_preflight() {
        let directory = tempfile::tempdir().unwrap();
        let source = directory.path().join("staging");
        let output = directory.path().join("output");
        fs::create_dir(&source).unwrap();
        fs::write(source.join("model"), "verified").unwrap();
        fs::create_dir(&output).unwrap();
        assert!(publish_directory(&source, &output).is_err());
        assert!(!output.join("model").exists());
        assert!(source.join("model").exists());
    }
}
