use anyhow::{Context as _, Result, ensure};
use std::{
    fs::{self, OpenOptions},
    io::Write as _,
    path::{Path, PathBuf},
};

/// Fixed width of a NUL-padded UTF-8 index key.
pub(crate) const HEADWORD_SIZE: usize = 32;
const RECORD_SIZE: usize = 40;
const STRIDE: u32 = 48;
const SPX_HEADER_SIZE: usize = 32;
const OFFSET_FIELD: std::ops::Range<usize> = HEADWORD_SIZE..HEADWORD_SIZE + 4;
const LENGTH_FIELD: std::ops::Range<usize> = OFFSET_FIELD.end..OFFSET_FIELD.end + 2;
const SUFFIXES: [&str; 3] = ["idx", "dat", "spx"];

/// One lookup key and its definition, ranking, and grammatical flags.
pub(crate) struct Record {
    pub headword: String,
    pub definition: Vec<u8>,
    pub priority: u8,
    pub flags: u8,
}

fn sidecar(idx: &[u8]) -> Result<Vec<u8>> {
    ensure!(
        idx.len().is_multiple_of(RECORD_SIZE),
        "idx size is not record-size divisible"
    );
    let count = u32::try_from(idx.len() / RECORD_SIZE).context("Too many dictionary records")?;
    let fine_count = count.div_ceil(STRIDE);
    let mut spx = Vec::with_capacity(SPX_HEADER_SIZE + fine_count as usize * HEADWORD_SIZE);
    spx.extend_from_slice(b"CPSPX1\0\0");
    for value in [1, STRIDE, count, fine_count, 0, 0] {
        spx.extend_from_slice(&value.to_le_bytes());
    }
    for record in idx
        .as_chunks::<RECORD_SIZE>()
        .0
        .iter()
        .step_by(STRIDE as usize)
    {
        spx.extend_from_slice(&record[..HEADWORD_SIZE]);
    }
    Ok(spx)
}

fn validate_sidecar(idx: &[u8], spx: &[u8]) -> Result<()> {
    ensure!(spx == sidecar(idx)?, "spx does not match idx");
    Ok(())
}

/// Rebuild sidecars without needing the original source or reading definition data.
#[expect(
    clippy::print_stdout,
    reason = "The CLI reports rebuilt and missing indexes."
)]
pub(crate) fn rebuild_sparse_indexes(directory: &Path) -> Result<()> {
    for name in ["vocab", "names", "grammar", "jmdict", "jmnedict"] {
        let idx_path = directory.join(format!("{name}.idx"));
        if !exists(&idx_path)? {
            println!("skip {name}: no {}", idx_path.display());
            continue;
        }
        let idx = read_file(&idx_path)?;
        let spx = sidecar(&idx).with_context(|| format!("Invalid index {}", idx_path.display()))?;
        let temporary = directory.join(format!("{name}.spx.tmp"));
        let destination = directory.join(format!("{name}.spx"));
        // Exclusive creation preserves any existing recovery file, including symlinks.
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temporary)
            .with_context(|| {
                format!(
                    "Cannot create {}; inspect any stale temporary file",
                    temporary.display()
                )
            })?;
        let result = (|| -> Result<()> {
            file.write_all(&spx)
                .with_context(|| format!("Cannot write {}", temporary.display()))?;
            file.sync_all()
                .with_context(|| format!("Cannot sync {}", temporary.display()))?;
            drop(file);
            validate_sidecar(&idx, &read_file(&temporary)?)?;
            fs::rename(&temporary, &destination)
                .with_context(|| format!("Cannot publish {}", destination.display()))?;
            Ok(())
        })();
        if let Err(error) = result {
            fs::remove_file(&temporary)
                .with_context(|| format!("{error:#}; cannot remove {}", temporary.display()))?;
            return Err(error);
        }
        println!(
            "{name}: {} records -> {} checkpoints ({} bytes, stride={STRIDE})",
            idx.len() / RECORD_SIZE,
            (spx.len() - SPX_HEADER_SIZE) / HEADWORD_SIZE,
            spx.len()
        );
    }
    Ok(())
}

fn validate(payloads: &[Vec<u8>; 3]) -> Result<()> {
    let [idx, dat, spx] = payloads;
    ensure!(
        idx.len().is_multiple_of(RECORD_SIZE),
        "idx size is not record-size divisible"
    );
    let mut previous: Option<&[u8]> = None;
    for (index, record) in idx.as_chunks::<RECORD_SIZE>().0.iter().enumerate() {
        let headword = &record[..HEADWORD_SIZE];
        let nul = headword
            .iter()
            .position(|&byte| byte == 0)
            .context("Headword lacks NUL padding")?;
        ensure!(
            headword[nul..].iter().all(|&byte| byte == 0),
            "idx record {index} has embedded NUL"
        );
        let key = &headword[..nul];
        ensure!(
            previous.is_none_or(|p| key >= p),
            "idx record {index} is out of order"
        );
        let offset = u64::from(u32::from_le_bytes(record[OFFSET_FIELD].try_into()?));
        let length = u64::from(u16::from_le_bytes(record[LENGTH_FIELD].try_into()?));
        ensure!(
            offset + length <= dat.len() as u64,
            "idx record {index} exceeds dat"
        );
        previous = Some(key);
    }
    validate_sidecar(idx, spx)?;
    Ok(())
}

#[expect(
    clippy::print_stdout,
    reason = "The CLI reports generated files and truncation counts."
)]
pub(crate) fn write(mut records: Vec<Record>, output: &Path, name: &str) -> Result<()> {
    records.sort_by(|a, b| a.headword.as_bytes().cmp(b.headword.as_bytes()));
    let mut idx = Vec::with_capacity(
        records
            .len()
            .checked_mul(RECORD_SIZE)
            .context("Index size overflow")?,
    );
    let mut dat = Vec::new();
    let mut previous: Option<(usize, usize)> = None;
    let mut truncated = 0;
    for record in records {
        ensure!(
            record.headword.len() < HEADWORD_SIZE,
            "Headword exceeds 31 bytes"
        );
        let (offset, length) = if let Some((offset, length)) =
            previous.filter(|&(offset, length)| dat[offset..offset + length] == record.definition)
        {
            (offset, length)
        } else {
            let length = record.definition.len().min(u16::MAX as usize);
            if length < record.definition.len() {
                truncated += 1;
            }
            let offset = dat.len();
            ensure!(
                u32::try_from((offset as u64) + (length as u64)).is_ok(),
                "Dictionary data exceeds the 4 GiB format limit"
            );
            dat.extend_from_slice(&record.definition[..length]);
            previous = Some((offset, length));
            (offset, length)
        };
        let mut key = [0; HEADWORD_SIZE];
        key[..record.headword.len()].copy_from_slice(record.headword.as_bytes());
        idx.extend_from_slice(&key);
        idx.extend_from_slice(&u32::try_from(offset)?.to_le_bytes());
        idx.extend_from_slice(&u16::try_from(length)?.to_le_bytes());
        idx.extend_from_slice(&[record.priority, record.flags]);
    }
    let spx = sidecar(&idx)?;
    let payloads = [idx, dat, spx];
    validate(&payloads)?;
    publish(output, name, &payloads, |source, destination| {
        fs::rename(source, destination)
    })?;
    for (suffix, bytes) in SUFFIXES.iter().zip(&payloads) {
        println!(
            "  {}: {} bytes",
            output.join(format!("{name}.{suffix}")).display(),
            bytes.len()
        );
    }
    if truncated > 0 {
        println!("Truncated {truncated} definitions > 65535 bytes");
    }
    Ok(())
}

fn read_file(path: &Path) -> Result<Vec<u8>> {
    fs::read(path).with_context(|| format!("Cannot read {}", path.display()))
}

fn read_set(paths: [&Path; 3]) -> Result<[Vec<u8>; 3]> {
    Ok([
        read_file(paths[0])?,
        read_file(paths[1])?,
        read_file(paths[2])?,
    ])
}

fn exists(path: &Path) -> Result<bool> {
    match fs::symlink_metadata(path) {
        Ok(_) => Ok(true),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(false),
        Err(error) => Err(error).with_context(|| format!("Cannot inspect {}", path.display())),
    }
}

struct PublicationSlot {
    final_path: PathBuf,
    temporary_path: PathBuf,
    backup_path: PathBuf,
    temporary_created: bool,
    original_backed_up: bool,
    published: bool,
}

impl PublicationSlot {
    fn new(output: &Path, name: &str, suffix: &str) -> Self {
        Self {
            final_path: output.join(format!("{name}.{suffix}")),
            temporary_path: output.join(format!("{name}.{suffix}.tmp")),
            backup_path: output.join(format!("{name}.{suffix}.bak")),
            temporary_created: false,
            original_backed_up: false,
            published: false,
        }
    }
}

fn rename_file(
    source: &Path,
    destination: &Path,
    rename: &mut impl FnMut(&Path, &Path) -> std::io::Result<()>,
) -> Result<()> {
    rename(source, destination).with_context(|| {
        format!(
            "Cannot rename {} to {}",
            source.display(),
            destination.display()
        )
    })
}

#[expect(
    clippy::print_stderr,
    reason = "Backup cleanup failures must be visible without rolling back published files."
)]
fn publish(
    output: &Path,
    name: &str,
    payloads: &[Vec<u8>; 3],
    mut rename: impl FnMut(&Path, &Path) -> std::io::Result<()>,
) -> Result<()> {
    fs::create_dir_all(output).with_context(|| format!("Cannot create {}", output.display()))?;
    let mut slots = SUFFIXES.map(|suffix| PublicationSlot::new(output, name, suffix));
    for slot in &slots {
        for path in [&slot.temporary_path, &slot.backup_path] {
            ensure!(
                !exists(path)?,
                "Stale recovery file exists: {}; inspect it before removing it",
                path.display()
            );
        }
        if exists(&slot.final_path)? {
            let metadata = fs::symlink_metadata(&slot.final_path)
                .with_context(|| format!("Cannot inspect {}", slot.final_path.display()))?;
            ensure!(
                metadata.file_type().is_file(),
                "Output is not a regular file: {}",
                slot.final_path.display()
            );
        }
    }

    let result = (|| -> Result<()> {
        for (slot, payload) in slots.iter_mut().zip(payloads) {
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&slot.temporary_path)
                .with_context(|| format!("Cannot create {}", slot.temporary_path.display()))?;

            slot.temporary_created = true;
            file.write_all(payload)
                .with_context(|| format!("Cannot write {}", slot.temporary_path.display()))?;
            file.sync_all()
                .with_context(|| format!("Cannot sync {}", slot.temporary_path.display()))?;
        }
        validate(&read_set(
            slots.each_ref().map(|slot| slot.temporary_path.as_path()),
        )?)
        .with_context(|| format!("Invalid temporary dictionary set in {}", output.display()))?;
        for slot in &mut slots {
            if exists(&slot.final_path)? {
                rename_file(&slot.final_path, &slot.backup_path, &mut rename)?;
                slot.original_backed_up = true;
            }
        }
        for slot in &mut slots {
            rename_file(&slot.temporary_path, &slot.final_path, &mut rename)?;
            slot.temporary_created = false;
            slot.published = true;
        }
        validate(&read_set(
            slots.each_ref().map(|slot| slot.final_path.as_path()),
        )?)
        .with_context(|| format!("Invalid published dictionary set in {}", output.display()))?;
        Ok(())
    })();
    if let Err(error) = result {
        let mut recovery_errors = Vec::new();
        for slot in &slots {
            if slot.published
                && let Err(error) = fs::remove_file(&slot.final_path)
            {
                recovery_errors.push(format!(
                    "Cannot remove {}: {error}",
                    slot.final_path.display()
                ));
            }
            if slot.original_backed_up
                && let Err(error) = rename_file(&slot.backup_path, &slot.final_path, &mut rename)
            {
                recovery_errors.push(format!("{error:#}"));
            }
            if slot.temporary_created
                && let Err(error) = fs::remove_file(&slot.temporary_path)
            {
                recovery_errors.push(format!(
                    "Cannot remove {}: {error}",
                    slot.temporary_path.display()
                ));
            }
        }
        return if recovery_errors.is_empty() {
            Err(error).context("Publication failed; previous files restored")
        } else {
            Err(error).context(format!(
                "Publication failed; inspect .bak/.tmp files in {}: {}",
                output.display(),
                recovery_errors.join("; ")
            ))
        };
    }
    for slot in &slots {
        if slot.original_backed_up
            && let Err(error) = fs::remove_file(&slot.backup_path)
        {
            eprintln!(
                "WARNING: published output; retained backup {}: {error}",
                slot.backup_path.display()
            );
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::tempdir;

    #[test]
    fn pinned_goldens_have_valid_records_and_sidecars() -> Result<()> {
        let root =
            Path::new(env!("CARGO_MANIFEST_DIR")).join("../../test/japanese_dict_converter/golden");
        for fixture in ["mini_jmdict", "mini_yomitan"] {
            let paths = SUFFIXES.map(|suffix| root.join(fixture).join(format!("vocab.{suffix}")));
            validate(&read_set(paths.each_ref().map(PathBuf::as_path))?)?;
        }
        Ok(())
    }

    #[test]
    fn sidecar_validator_rejects_header_checkpoint_and_size_corruption() -> Result<()> {
        let mut idx = vec![0; 97 * RECORD_SIZE];
        for (i, record) in idx.as_chunks_mut::<RECORD_SIZE>().0.iter_mut().enumerate() {
            record[..3].copy_from_slice(format!("{i:03}").as_bytes());
        }
        let valid = sidecar(&idx)?;
        validate_sidecar(&idx, &valid)?;
        for offset in [0, 8, 12, 16, 20, 24, 28, 32, 64, 96] {
            let mut corrupt = valid.clone();
            corrupt[offset] ^= 1;
            assert!(
                validate_sidecar(&idx, &corrupt).is_err(),
                "corruption at {offset} accepted"
            );
        }
        for length in [0, 31, valid.len() - 1] {
            assert!(
                validate_sidecar(&idx, &valid[..length]).is_err(),
                "truncated sidecar accepted"
            );
        }
        let mut extended = valid.clone();
        extended.push(0);
        assert!(
            validate_sidecar(&idx, &extended).is_err(),
            "trailing data accepted"
        );
        assert!(
            validate_sidecar(&idx[..idx.len() - 1], &valid).is_err(),
            "partial index record accepted"
        );
        Ok(())
    }

    #[test]
    fn failed_rename_restores_old_set_at_each_publication_step() {
        for fail_at in 0..6 {
            let temp = tempdir().expect("create temporary output directory");
            let old = [
                b"old index".to_vec(),
                b"old data".to_vec(),
                b"old sidecar".to_vec(),
            ];
            let finals = SUFFIXES.map(|s| temp.path().join(format!("vocab.{s}")));
            for (path, bytes) in finals.iter().zip(&old) {
                fs::write(path, bytes).expect("write previous dictionary files");
            }
            let payloads = [
                vec![],
                vec![],
                sidecar(&[]).expect("build empty dictionary sidecar"),
            ];
            let mut calls = 0;
            let result = publish(temp.path(), "vocab", &payloads, |source, destination| {
                let fail = calls == fail_at;
                calls += 1;
                if fail {
                    Err(std::io::Error::other("injected rename failure"))
                } else {
                    fs::rename(source, destination)
                }
            });
            let error = result.expect_err("injected publication failure");
            let error_text = format!("{error:#}");
            let suffix = SUFFIXES[fail_at % 3];
            let source = if fail_at < 3 {
                format!("vocab.{suffix}")
            } else {
                format!("vocab.{suffix}.tmp")
            };
            let destination = if fail_at < 3 {
                format!("vocab.{suffix}.bak")
            } else {
                format!("vocab.{suffix}")
            };
            assert!(
                error_text.contains(&source) && error_text.contains(&destination),
                "step {fail_at}: {error_text}"
            );
            assert_eq!(
                read_set(finals.each_ref().map(PathBuf::as_path))
                    .expect("read restored dictionary files"),
                old,
                "step {fail_at}"
            );
            assert_eq!(
                fs::read_dir(temp.path())
                    .expect("inspect publication artifacts")
                    .count(),
                3,
                "step {fail_at}: recovery left unexpected files"
            );
        }
    }

    #[test]
    fn empty_dictionary_has_valid_header() {
        let expected = b"CPSPX1\0\0\x01\0\0\0\x30\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";
        assert_eq!(
            sidecar(&[]).expect("build empty dictionary sidecar"),
            expected
        );
    }
    #[test]
    fn read_set_error_identifies_missing_file() -> Result<()> {
        let temp = tempdir()?;
        let paths = SUFFIXES.map(|suffix| temp.path().join(format!("vocab.{suffix}")));
        fs::write(&paths[0], b"")?;
        let error =
            read_set(paths.each_ref().map(PathBuf::as_path)).expect_err("data file is missing");
        assert!(
            format!("{error:#}").contains(&paths[1].display().to_string()),
            "{error:#}"
        );
        Ok(())
    }
}
