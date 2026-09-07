// SPDX-License-Identifier: MIT
use anyhow::{Context, Result, bail};
use std::cmp::Ordering;
use std::collections::HashSet;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Component, Path, PathBuf};

/// Maximum number of source images accepted from one input.
pub const MAX_IMAGE_FILES: usize = 10_000;
/// Maximum uncompressed size of one source image (256 MiB).
pub const MAX_ENTRY_BYTES: u64 = 256 * 1024 * 1024;
/// Maximum total uncompressed size of all source images (4 GiB).
pub const MAX_TOTAL_BYTES: u64 = 4 * 1024 * 1024 * 1024;

/// Copies supported source images into `destination` and returns their absolute
/// paths in deterministic natural relative-path order.
///
/// Directory trees and archive folders are preserved so repeated basenames in
/// different chapters remain distinct. ZIP/CBZ and RAR/CBR inputs are bounded
/// by [`MAX_IMAGE_FILES`], [`MAX_ENTRY_BYTES`], and [`MAX_TOTAL_BYTES`].
///
/// # Errors
///
/// Returns an error for unsupported inputs, empty inputs, unsafe archive paths,
/// links, duplicate normalized paths, corrupt archives, or exceeded limits.
pub fn collect(input: &Path, destination: &Path) -> Result<Vec<PathBuf>> {
    let metadata = fs::symlink_metadata(input)
        .with_context(|| format!("inspect input {}", input.display()))?;
    if metadata.file_type().is_symlink() {
        bail!("symbolic link is not allowed as input: {}", input.display());
    }
    if metadata.is_dir() {
        return collect_folder(input, destination);
    }
    if !metadata.is_file() {
        bail!("unsupported input type: {}", input.display());
    }

    match lowercase_extension(input).as_deref() {
        Some("zip" | "cbz") => collect_zip(input, destination),
        Some("rar" | "cbr") => collect_rar(input, destination),
        _ => bail!(
            "unsupported input type: {}; expected a folder, CBZ/ZIP, or CBR/RAR archive",
            input.display()
        ),
    }
}

fn collect_folder(input: &Path, destination: &Path) -> Result<Vec<PathBuf>> {
    let root = fs::canonicalize(input)
        .with_context(|| format!("resolve input folder {}", input.display()))?;
    if destination.exists() {
        let resolved_destination = fs::canonicalize(destination)
            .with_context(|| format!("resolve destination {}", destination.display()))?;
        if resolved_destination.starts_with(&root) {
            bail!("destination must not be inside the input folder");
        }
    }

    let mut sources = Vec::new();
    visit_folder(&root, &root, &mut sources)?;
    validate_source_sizes(sources.iter().map(|(source, _)| {
        fs::metadata(source)
            .map(|metadata| metadata.len())
            .with_context(|| format!("inspect source image {}", source.display()))
    }))?;
    if sources.is_empty() {
        bail!("no supported image files found in {}", input.display());
    }
    sources.sort_by(|(_, left), (_, right)| natural_path_cmp(left, right));

    let destination = prepare_destination(destination)?;
    let mut results = Vec::with_capacity(sources.len());
    let mut actual_total = 0_u64;
    for (source, relative) in sources {
        let resolved_source = fs::canonicalize(&source)
            .with_context(|| format!("resolve source image {}", source.display()))?;
        if !resolved_source.starts_with(&root) {
            bail!(
                "source image escaped the input folder: {}",
                source.display()
            );
        }
        let declared = fs::metadata(&resolved_source)
            .with_context(|| format!("inspect source image {}", source.display()))?
            .len();
        let target = destination.join(relative);
        copy_reader_bounded(
            File::open(&resolved_source)
                .with_context(|| format!("open source image {}", source.display()))?,
            &target,
            declared,
            &mut actual_total,
        )?;
        results.push(target);
    }
    Ok(results)
}

fn visit_folder(
    root: &Path,
    directory: &Path,
    sources: &mut Vec<(PathBuf, PathBuf)>,
) -> Result<()> {
    for entry in fs::read_dir(directory)
        .with_context(|| format!("read input folder {}", directory.display()))?
    {
        let entry = entry.with_context(|| format!("read entry in {}", directory.display()))?;
        let path = entry.path();
        let metadata = fs::symlink_metadata(&path)
            .with_context(|| format!("inspect input path {}", path.display()))?;
        if metadata.file_type().is_symlink() {
            bail!("symbolic link is not allowed in input: {}", path.display());
        }
        if metadata.is_dir() {
            visit_folder(root, &path, sources)?;
        } else if metadata.is_file() && is_supported_image(&path) {
            let relative = path
                .strip_prefix(root)
                .with_context(|| format!("make {} relative to input", path.display()))?
                .to_path_buf();
            validate_relative_path(&relative)?;
            sources.push((path, relative));
            if sources.len() > MAX_IMAGE_FILES {
                bail!("input exceeds the {MAX_IMAGE_FILES} image limit");
            }
        }
    }
    Ok(())
}

fn collect_zip(input: &Path, destination: &Path) -> Result<Vec<PathBuf>> {
    let file =
        File::open(input).with_context(|| format!("open ZIP archive {}", input.display()))?;
    let mut archive = zip::ZipArchive::new(file)
        .with_context(|| format!("open ZIP archive {}", input.display()))?;
    let mut entries = Vec::new();
    let mut seen = HashSet::new();
    let mut declared_total = 0_u64;

    for index in 0..archive.len() {
        let entry = archive
            .by_index(index)
            .with_context(|| format!("read ZIP header {index} from {}", input.display()))?;
        let Some((relative, normalized)) = normalize_archive_path(entry.name())? else {
            continue;
        };
        if !seen.insert(normalized.clone()) {
            bail!("duplicate archive path: {normalized}");
        }
        if entry
            .unix_mode()
            .is_some_and(|mode| mode & 0o170000 == 0o120000)
        {
            bail!("archive symbolic link is not allowed: {normalized}");
        }
        if entry.is_dir() || !is_supported_image(&relative) {
            continue;
        }
        check_declared_entry(entry.size(), &mut declared_total, entries.len() + 1)?;
        entries.push((index, relative, entry.size()));
    }
    if entries.is_empty() {
        bail!("no supported image files found in {}", input.display());
    }
    entries.sort_by(|(_, left, _), (_, right, _)| natural_path_cmp(left, right));

    let destination = prepare_destination(destination)?;
    let mut results = Vec::with_capacity(entries.len());
    let mut actual_total = 0_u64;
    for (index, relative, declared) in entries {
        let entry = archive
            .by_index(index)
            .with_context(|| format!("open ZIP entry {}", relative.display()))?;
        let target = destination.join(relative);
        copy_reader_bounded(entry, &target, declared, &mut actual_total)
            .with_context(|| format!("extract ZIP entry {}", target.display()))?;
        results.push(target);
    }
    Ok(results)
}

#[cfg(not(crossink_rar_helper))]
fn collect_rar(input: &Path, destination: &Path) -> Result<Vec<PathBuf>> {
    let helper = if let Some(path) = std::env::var_os("CROSSINK_RAR_HELPER") {
        PathBuf::from(path)
    } else {
        let name = if cfg!(windows) {
            "crossink-rar.exe"
        } else {
            "crossink-rar"
        };
        std::env::current_exe()
            .context("locate RAR helper beside executable")?
            .with_file_name(name)
    };
    collect_rar_with_helper(input, destination, &helper)
}

#[cfg(not(crossink_rar_helper))]
fn collect_rar_with_helper(
    input: &Path,
    destination: &Path,
    helper: &Path,
) -> Result<Vec<PathBuf>> {
    if !helper.is_file() {
        bail!(
            "RAR helper missing: {}; install crossink-rar beside crossink-manga or set CROSSINK_RAR_HELPER",
            helper.display()
        );
    }
    let helper = fs::canonicalize(helper).context("resolve RAR helper executable")?;
    // UnRAR runs in a separate program. Only ordinary extracted files cross this boundary.
    let extracted = tempfile::tempdir().context("create temporary RAR extraction directory")?;
    let status = std::process::Command::new(&helper)
        .arg(input)
        .arg(extracted.path())
        .stdin(std::process::Stdio::null())
        .status()
        .with_context(|| format!("start RAR helper {}", helper.display()))?;
    if !status.success() {
        bail!("RAR helper failed with status {status}");
    }
    // Revalidate sizes, names and links rather than trusting helper-produced paths.
    collect_folder(extracted.path(), destination).context("validate RAR helper output")
}

#[cfg(crossink_rar_helper)]
fn collect_rar(input: &Path, destination: &Path) -> Result<Vec<PathBuf>> {
    let destination = prepare_destination(destination)?;
    let mut archive = unrar::Archive::new(input)
        .open_for_processing()
        .with_context(|| format!("open RAR archive {}", input.display()))?;
    let mut seen = HashSet::new();
    let mut results = Vec::new();
    let mut declared_total = 0_u64;
    let mut actual_total = 0_u64;

    loop {
        let Some(entry) = archive
            .read_header()
            .with_context(|| format!("read RAR archive {}", input.display()))?
        else {
            break;
        };
        let header = entry.entry();
        let display_name = header.filename.to_string_lossy();
        let Some((relative, normalized)) = normalize_archive_path(&display_name)? else {
            archive = entry
                .skip()
                .with_context(|| format!("read RAR archive {}", input.display()))?;
            continue;
        };
        if !seen.insert(normalized.clone()) {
            bail!("duplicate archive path: {normalized}");
        }
        if header.file_attr & 0o170000 == 0o120000 {
            bail!("archive symbolic link is not allowed: {normalized}");
        }
        if header.is_split() {
            bail!("multipart RAR entries are not supported: {normalized}");
        }
        if header.is_encrypted() {
            bail!("encrypted RAR entries are not supported: {normalized}");
        }
        let is_image = header.is_file() && is_supported_image(&relative);
        let declared = header.unpacked_size;
        if !is_image {
            archive = entry
                .skip()
                .with_context(|| format!("read RAR archive {}", input.display()))?;
            continue;
        }
        check_declared_entry(declared, &mut declared_total, results.len() + 1)?;
        let (data, next) = entry
            .read()
            .with_context(|| format!("read RAR entry {normalized}"))?;
        archive = next;
        let actual = u64::try_from(data.len()).context("RAR entry size does not fit in u64")?;
        check_actual_entry(actual, declared, &mut actual_total)?;
        let target = destination.join(relative);
        write_owned(&target, &data)?;
        results.push(target);
    }
    if results.is_empty() {
        bail!("no supported image files found in {}", input.display());
    }
    results.sort_by(|left, right| {
        let left = left.strip_prefix(&destination).unwrap_or(left);
        let right = right.strip_prefix(&destination).unwrap_or(right);
        natural_path_cmp(left, right)
    });
    Ok(results)
}

fn validate_source_sizes<I>(sizes: I) -> Result<()>
where
    I: IntoIterator<Item = Result<u64>>,
{
    let mut total = 0_u64;
    let mut count = 0_usize;
    for size in sizes {
        count += 1;
        check_declared_entry(size?, &mut total, count)?;
    }
    Ok(())
}

fn check_declared_entry(size: u64, total: &mut u64, count: usize) -> Result<()> {
    if count > MAX_IMAGE_FILES {
        bail!("input exceeds the {MAX_IMAGE_FILES} image limit");
    }
    if size > MAX_ENTRY_BYTES {
        bail!("image entry exceeds the {MAX_ENTRY_BYTES} byte limit");
    }
    *total = total
        .checked_add(size)
        .context("declared archive expansion size overflow")?;
    if *total > MAX_TOTAL_BYTES {
        bail!("input exceeds the {MAX_TOTAL_BYTES} total expanded byte limit");
    }
    Ok(())
}

fn check_actual_entry(actual: u64, declared: u64, total: &mut u64) -> Result<()> {
    if actual > MAX_ENTRY_BYTES {
        bail!("image entry exceeds the {MAX_ENTRY_BYTES} actual byte limit");
    }
    if actual != declared {
        bail!("image entry size mismatch: declared {declared} bytes, read {actual} bytes");
    }
    *total = total
        .checked_add(actual)
        .context("actual archive expansion size overflow")?;
    if *total > MAX_TOTAL_BYTES {
        bail!("input exceeds the {MAX_TOTAL_BYTES} actual expanded byte limit");
    }
    Ok(())
}

fn copy_reader_bounded<R: Read>(
    mut reader: R,
    target: &Path,
    declared: u64,
    actual_total: &mut u64,
) -> Result<()> {
    if let Some(parent) = target.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("create image directory {}", parent.display()))?;
    }
    let mut output = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(target)
        .with_context(|| format!("create owned image {}", target.display()))?;
    let result = (|| {
        let mut actual = 0_u64;
        let mut buffer = vec![0_u8; 64 * 1024];
        loop {
            let read = reader
                .read(&mut buffer)
                .with_context(|| format!("read image data for {}", target.display()))?;
            if read == 0 {
                break;
            }
            actual = actual
                .checked_add(u64::try_from(read).context("read size does not fit in u64")?)
                .context("actual image size overflow")?;
            if actual > declared || actual > MAX_ENTRY_BYTES {
                bail!("image entry exceeded its declared or configured byte limit");
            }
            output
                .write_all(&buffer[..read])
                .with_context(|| format!("write owned image {}", target.display()))?;
        }
        output
            .flush()
            .with_context(|| format!("flush owned image {}", target.display()))?;
        check_actual_entry(actual, declared, actual_total)
    })();
    if result.is_err() {
        drop(output);
        let _ = fs::remove_file(target);
    }
    result
}

#[cfg(crossink_rar_helper)]
fn write_owned(target: &Path, data: &[u8]) -> Result<()> {
    if let Some(parent) = target.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("create image directory {}", parent.display()))?;
    }
    let mut output = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(target)
        .with_context(|| format!("create owned image {}", target.display()))?;
    output
        .write_all(data)
        .with_context(|| format!("write owned image {}", target.display()))
}

fn prepare_destination(destination: &Path) -> Result<PathBuf> {
    let absolute = std::path::absolute(destination)
        .with_context(|| format!("resolve destination {}", destination.display()))?;
    fs::create_dir_all(&absolute)
        .with_context(|| format!("create destination {}", absolute.display()))?;
    Ok(absolute)
}

fn normalize_archive_path(name: &str) -> Result<Option<(PathBuf, String)>> {
    if name.contains('\0') {
        bail!("unsafe archive path contains NUL");
    }
    let normalized_slashes = name.replace('\\', "/");
    if normalized_slashes.starts_with('/')
        || normalized_slashes
            .as_bytes()
            .get(1)
            .is_some_and(|byte| *byte == b':')
    {
        bail!("unsafe archive path: {name}");
    }

    let mut parts = Vec::new();
    for part in normalized_slashes.split('/') {
        match part {
            "" | "." => {}
            ".." => bail!("unsafe archive path: {name}"),
            _ => parts.push(part),
        }
    }
    if parts.is_empty() {
        return Ok(None);
    }
    let normalized = parts.join("/");
    Ok(Some((parts.iter().collect(), normalized)))
}

fn validate_relative_path(path: &Path) -> Result<()> {
    if path.as_os_str().is_empty()
        || path
            .components()
            .any(|component| !matches!(component, Component::Normal(_)))
    {
        bail!("unsafe relative input path: {}", path.display());
    }
    Ok(())
}

fn is_supported_image(path: &Path) -> bool {
    matches!(
        lowercase_extension(path).as_deref(),
        Some("jpg" | "jpeg" | "png" | "webp" | "bmp")
    )
}

fn lowercase_extension(path: &Path) -> Option<String> {
    path.extension()
        .and_then(|extension| extension.to_str())
        .map(str::to_ascii_lowercase)
}

fn natural_path_cmp(left: &Path, right: &Path) -> Ordering {
    let left = left.to_string_lossy().replace('\\', "/");
    let right = right.to_string_lossy().replace('\\', "/");
    natural_str_cmp(&left, &right).then_with(|| left.cmp(&right))
}

fn natural_str_cmp(left: &str, right: &str) -> Ordering {
    let left = left.as_bytes();
    let right = right.as_bytes();
    let (mut left_at, mut right_at) = (0, 0);
    while left_at < left.len() && right_at < right.len() {
        if left[left_at].is_ascii_digit() && right[right_at].is_ascii_digit() {
            let left_end = digit_end(left, left_at);
            let right_end = digit_end(right, right_at);
            let left_digits = trim_zeroes(&left[left_at..left_end]);
            let right_digits = trim_zeroes(&right[right_at..right_end]);
            let ordering = left_digits
                .len()
                .cmp(&right_digits.len())
                .then_with(|| left_digits.cmp(right_digits));
            if ordering != Ordering::Equal {
                return ordering;
            }
            left_at = left_end;
            right_at = right_end;
            continue;
        }
        let ordering = left[left_at]
            .to_ascii_lowercase()
            .cmp(&right[right_at].to_ascii_lowercase());
        if ordering != Ordering::Equal {
            return ordering;
        }
        left_at += 1;
        right_at += 1;
    }
    left.len().cmp(&right.len())
}

fn digit_end(value: &[u8], mut at: usize) -> usize {
    while at < value.len() && value[at].is_ascii_digit() {
        at += 1;
    }
    at
}

fn trim_zeroes(mut value: &[u8]) -> &[u8] {
    while value.len() > 1 && value.first() == Some(&b'0') {
        value = &value[1..];
    }
    value
}

#[cfg(test)]
mod tests {
    use super::{
        MAX_ENTRY_BYTES, MAX_IMAGE_FILES, MAX_TOTAL_BYTES, check_actual_entry,
        check_declared_entry, collect,
    };
    use std::fs;
    use std::io::Write;
    use std::path::{Path, PathBuf};
    use tempfile::TempDir;
    use zip::write::SimpleFileOptions;

    fn write_zip(path: &Path, entries: &[(&str, &[u8])]) {
        let file = fs::File::create(path).expect("create zip fixture");
        let mut archive = zip::ZipWriter::new(file);
        for (name, bytes) in entries {
            archive
                .start_file(*name, SimpleFileOptions::default())
                .expect("start zip entry");
            archive.write_all(bytes).expect("write zip entry");
        }
        archive.finish().expect("finish zip fixture");
    }

    fn relative_paths(root: &Path, paths: &[PathBuf]) -> Vec<PathBuf> {
        paths
            .iter()
            .map(|path| {
                path.strip_prefix(root)
                    .expect("path under destination")
                    .into()
            })
            .collect()
    }

    #[test]
    fn folder_should_copy_images_in_natural_relative_path_order() {
        let fixture = TempDir::new().expect("fixture directory");
        let input = fixture.path().join("input");
        let destination = fixture.path().join("owned");
        fs::create_dir_all(input.join("chapter10")).expect("chapter10");
        fs::create_dir_all(input.join("chapter2")).expect("chapter2");
        fs::write(input.join("chapter10/page1.PNG"), b"ten").expect("page");
        fs::write(input.join("chapter2/page10.webp"), b"two-ten").expect("page");
        fs::write(input.join("chapter2/page2.jpg"), b"two-two").expect("page");
        fs::write(input.join("chapter2/notes.txt"), b"ignored").expect("notes");

        let result = collect(&input, &destination).expect("collect folder");

        assert_eq!(
            relative_paths(&destination, &result),
            vec![
                PathBuf::from("chapter2/page2.jpg"),
                PathBuf::from("chapter2/page10.webp"),
                PathBuf::from("chapter10/page1.PNG"),
            ]
        );
        assert!(result.iter().all(|path| path.is_absolute()));
        assert_eq!(fs::read(&result[0]).expect("owned image"), b"two-two");
    }

    #[test]
    fn cbz_should_preserve_repeated_basenames_and_natural_order() {
        let fixture = TempDir::new().expect("fixture directory");
        let input = fixture.path().join("volume.cbz");
        let destination = fixture.path().join("owned");
        write_zip(
            &input,
            &[
                ("chapter10/1.png", b"ten"),
                ("chapter2/10.jpg", b"two-ten"),
                ("chapter2/2.jpg", b"two-two"),
                ("chapter2/readme.txt", b"ignored"),
            ],
        );

        let result = collect(&input, &destination).expect("collect cbz");

        assert_eq!(
            relative_paths(&destination, &result),
            vec![
                PathBuf::from("chapter2/2.jpg"),
                PathBuf::from("chapter2/10.jpg"),
                PathBuf::from("chapter10/1.png"),
            ]
        );
        assert_eq!(fs::read(&result[2]).expect("owned image"), b"ten");
    }

    #[test]
    fn cbz_should_reject_parent_traversal() {
        let fixture = TempDir::new().expect("fixture directory");
        let input = fixture.path().join("volume.cbz");
        let destination = fixture.path().join("owned");
        write_zip(&input, &[("../escape.png", b"escape")]);

        let error = collect(&input, &destination).expect_err("reject traversal");

        assert!(error.to_string().contains("unsafe archive path"));
        assert!(!fixture.path().join("escape.png").exists());
    }

    #[test]
    fn cbz_should_reject_duplicate_normalized_paths() {
        let fixture = TempDir::new().expect("fixture directory");
        let input = fixture.path().join("volume.zip");
        let destination = fixture.path().join("owned");
        write_zip(
            &input,
            &[
                ("chapter/page.png", b"first"),
                ("chapter\\page.png", b"second"),
            ],
        );

        let error = collect(&input, &destination).expect_err("reject duplicate");

        assert!(error.to_string().contains("duplicate archive path"));
    }

    #[test]
    fn corrupted_cbz_should_return_contextual_error() {
        let fixture = TempDir::new().expect("fixture directory");
        let input = fixture.path().join("broken.cbz");
        fs::write(&input, b"this is not a zip archive").expect("corrupt fixture");

        let error = collect(&input, &fixture.path().join("owned")).expect_err("reject corrupt zip");

        assert!(error.to_string().contains("open ZIP archive"));
    }

    #[test]
    fn zip_should_reject_archive_symlinks() {
        let fixture = TempDir::new().expect("fixture directory");
        let input = fixture.path().join("volume.cbz");
        let file = fs::File::create(&input).expect("create zip fixture");
        let mut archive = zip::ZipWriter::new(file);
        archive
            .add_symlink("linked.png", "../outside.png", SimpleFileOptions::default())
            .expect("add symlink");
        archive.finish().expect("finish zip fixture");

        let error = collect(&input, &fixture.path().join("owned")).expect_err("reject link");

        assert!(error.to_string().contains("symbolic link"));
    }

    #[test]
    fn extraction_bounds_should_check_declared_and_actual_sizes() {
        let mut declared_total = 0;
        let declared_error = check_declared_entry(
            MAX_ENTRY_BYTES + 1,
            &mut declared_total,
            MAX_IMAGE_FILES.min(1),
        )
        .expect_err("declared entry limit");
        assert!(declared_error.to_string().contains("byte limit"));

        let mut actual_total = MAX_TOTAL_BYTES;
        let actual_error =
            check_actual_entry(1, 1, &mut actual_total).expect_err("actual total limit");
        assert!(
            actual_error
                .to_string()
                .contains("actual expanded byte limit")
        );

        let mut actual_total = 0;
        let mismatch_error =
            check_actual_entry(2, 1, &mut actual_total).expect_err("declared/actual mismatch");
        assert!(mismatch_error.to_string().contains("size mismatch"));
    }

    #[cfg(not(crossink_rar_helper))]
    #[test]
    fn missing_rar_helper_should_report_actionable_error() {
        let fixture = TempDir::new().expect("fixture directory");
        let input = fixture.path().join("volume.cbr");
        fs::write(&input, b"rar").expect("input");
        let error = super::collect_rar_with_helper(
            &input,
            &fixture.path().join("owned"),
            &fixture.path().join("missing-helper"),
        )
        .expect_err("missing helper");
        assert!(format!("{error:#}").contains("RAR helper"));
    }

    #[cfg(all(unix, not(crossink_rar_helper)))]
    fn fake_rar_helper(root: &Path, script: &str) -> PathBuf {
        use std::os::unix::fs::PermissionsExt;
        let helper = root.join("helper with spaces");
        fs::write(&helper, format!("#!/bin/sh\n{script}\n")).expect("helper script");
        fs::set_permissions(&helper, fs::Permissions::from_mode(0o755)).expect("executable");
        helper
    }

    #[cfg(all(unix, not(crossink_rar_helper)))]
    #[test]
    fn rar_process_output_is_validated_and_naturally_ordered() {
        let fixture = TempDir::new().expect("fixture directory");
        let helper = fake_rar_helper(
            fixture.path(),
            r#"mkdir -p "$2/chapter"
printf 'ten' > "$2/chapter/10.png"
printf 'two' > "$2/chapter/2.png""#,
        );
        let destination = fixture.path().join("owned");
        let files = super::collect_rar_with_helper(
            Path::new("input with spaces.cbr"),
            &destination,
            &helper,
        )
        .expect("process output");
        assert_eq!(
            relative_paths(&destination, &files),
            vec![
                PathBuf::from("chapter/2.png"),
                PathBuf::from("chapter/10.png")
            ]
        );
        assert_eq!(fs::read(&files[0]).expect("copied"), b"two");
    }

    #[cfg(all(unix, not(crossink_rar_helper)))]
    #[test]
    fn rar_process_symlinks_and_failed_outputs_are_rejected() {
        let fixture = TempDir::new().expect("fixture directory");
        let destination = fixture.path().join("owned");
        let helper = fake_rar_helper(fixture.path(), r#"ln -s /dev/null "$2/link.png""#);
        let error = super::collect_rar_with_helper(Path::new("input.cbr"), &destination, &helper)
            .expect_err("reject link");
        assert!(format!("{error:#}").contains("symbolic link"));
        assert!(!destination.exists());
        let helper = fake_rar_helper(
            fixture.path(),
            r#"printf 'partial' > "$2/1.png"
exit 7"#,
        );
        let error = super::collect_rar_with_helper(Path::new("input.cbr"), &destination, &helper)
            .expect_err("reject failure");
        assert!(error.to_string().contains("RAR helper failed"));
        assert!(!destination.exists());
    }

    #[cfg(crossink_rar_helper)]
    #[test]
    fn actual_rar_fixture_should_extract_an_image_through_memory_api() {
        // unrar 0.5.8's 87-byte RAR4 VERSION fixture, with its seven-byte entry
        // name replaced by `aa.jpeg` and the RAR header CRC recalculated below.
        let mut rar = vec![
            0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x00, 0xcf, 0x90, 0x73, 0x00, 0x00, 0x0d, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x0c, 0x74, 0x20, 0x80, 0x27, 0x00, 0x15,
            0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x03, 0x45, 0xf3, 0x7d, 0xc6, 0xa4, 0x8a,
            0x07, 0x47, 0x1d, 0x33, 0x07, 0x00, 0xa4, 0x81, 0x00, 0x00, 0x56, 0x45, 0x52, 0x53,
            0x49, 0x4f, 0x4e, 0x0c, 0x00, 0x8f, 0xec, 0x8a, 0x45, 0xcc, 0x23, 0xc8, 0x48, 0x08,
            0x83, 0x62, 0xfe, 0x5f, 0xdd, 0x5c, 0x53, 0x88, 0xf0, 0x72, 0xc4, 0x3d, 0x7b, 0x00,
            0x40, 0x07, 0x00,
        ];
        rar[52..59].copy_from_slice(b"aa.jpeg");
        let checksum = crc32(&rar[22..59]);
        rar[20..22].copy_from_slice(&(checksum as u16).to_le_bytes());
        let fixture = TempDir::new().expect("fixture directory");
        let input = fixture.path().join("volume.cbr");
        let destination = fixture.path().join("owned");
        fs::write(&input, rar).expect("RAR fixture");

        let result = collect(&input, &destination).expect("collect CBR");

        assert_eq!(
            relative_paths(&destination, &result),
            vec![PathBuf::from("aa.jpeg")]
        );
        assert_eq!(fs::read(&result[0]).expect("RAR payload"), b"unrar-0.4.0");
    }

    #[cfg(crossink_rar_helper)]
    fn crc32(bytes: &[u8]) -> u32 {
        let mut crc = u32::MAX;
        for byte in bytes {
            crc ^= u32::from(*byte);
            for _ in 0..8 {
                crc = (crc >> 1) ^ (0xedb8_8320 & 0_u32.wrapping_sub(crc & 1));
            }
        }
        !crc
    }

    #[test]
    fn unsupported_input_file_should_return_clear_error() {
        let fixture = TempDir::new().expect("fixture directory");
        let input = fixture.path().join("volume.pdf");
        fs::write(&input, b"pdf").expect("fixture");

        let error = collect(&input, &fixture.path().join("owned")).expect_err("reject input");

        assert!(error.to_string().contains("unsupported input type"));
    }

    #[cfg(unix)]
    #[test]
    fn folder_should_reject_symlinks() {
        use std::os::unix::fs::symlink;

        let fixture = TempDir::new().expect("fixture directory");
        let input = fixture.path().join("input");
        fs::create_dir(&input).expect("input");
        fs::write(fixture.path().join("outside.png"), b"outside").expect("outside image");
        symlink(fixture.path().join("outside.png"), input.join("linked.png")).expect("symlink");

        let error = collect(&input, &fixture.path().join("owned")).expect_err("reject symlink");

        assert!(error.to_string().contains("symbolic link"));
    }

    #[cfg(unix)]
    #[test]
    fn top_level_input_should_reject_symlinks() {
        use std::os::unix::fs::symlink;

        let fixture = TempDir::new().expect("fixture directory");
        let actual = fixture.path().join("actual");
        let input = fixture.path().join("linked-input");
        fs::create_dir(&actual).expect("actual input");
        fs::write(actual.join("page.png"), b"page").expect("page");
        symlink(&actual, &input).expect("symlink input");

        let error = collect(&input, &fixture.path().join("owned")).expect_err("reject symlink");

        assert!(error.to_string().contains("symbolic link"));
    }
}
