use anyhow::Result;
use std::{
    fs,
    io::Write as _,
    path::{Path, PathBuf},
    process::{Command, Output},
};
use tempfile::tempdir;
use zip::{ZipWriter, write::SimpleFileOptions};

fn fixture() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../test/japanese_dict_converter/fixtures/mini_yomitan")
}

fn run(input: &Path, output: &Path, name: &str) -> Result<Output> {
    Ok(Command::new(env!("CARGO_BIN_EXE_crossink-dict"))
        .arg("--input")
        .arg(input)
        .arg("--output-dir")
        .arg(output)
        .args(["--name", name, "--format", "yomitan"])
        .output()?)
}

fn check_golden(output: &Path, name: &str) -> Result<()> {
    let golden = fixture().join("../../golden/mini_yomitan");
    for suffix in ["idx", "dat", "spx"] {
        assert_eq!(
            fs::read(output.join(format!("{name}.{suffix}")))?,
            fs::read(golden.join(format!("vocab.{suffix}")))?,
            "{suffix}"
        );
    }
    Ok(())
}

#[test]
fn directory_matches_golden_for_all_slots() -> Result<()> {
    let temp = tempdir()?;
    for name in ["vocab", "names", "grammar"] {
        let result = run(&fixture(), temp.path(), name)?;
        assert!(
            result.status.success(),
            "{}",
            String::from_utf8_lossy(&result.stderr)
        );
        check_golden(temp.path(), name)?;
    }
    Ok(())
}

#[test]
fn deflated_zip_matches_golden() -> Result<()> {
    let temp = tempdir()?;
    let source = temp.path().join("input.zip");
    let mut zip = ZipWriter::new(fs::File::create(&source)?);
    for name in ["index.json", "term_bank_1.json"] {
        zip.start_file(
            name,
            SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated),
        )?;
        zip.write_all(&fs::read(fixture().join(name))?)?;
    }
    zip.finish()?;
    let result = run(&source, &temp.path().join("out"), "vocab")?;
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    check_golden(&temp.path().join("out"), "vocab")?;
    Ok(())
}

#[test]
fn stale_recovery_files_are_preserved() -> Result<()> {
    for suffix in [
        "idx.tmp", "dat.tmp", "spx.tmp", "idx.bak", "dat.bak", "spx.bak",
    ] {
        let temp = tempdir()?;
        let stale = temp.path().join(format!("vocab.{suffix}"));
        fs::write(&stale, b"recover me")?;
        let result = run(&fixture(), temp.path(), "vocab")?;
        assert!(
            !result.status.success(),
            "stale {suffix} must reject publication"
        );
        assert_eq!(
            fs::read(&stale)?,
            b"recover me",
            "stale {suffix} must be preserved"
        );
        assert_eq!(
            fs::read_dir(temp.path())?.count(),
            1,
            "stale {suffix}: unexpected artifacts"
        );
    }
    Ok(())
}

#[test]
fn corrupt_input_preserves_previous_output() -> Result<()> {
    let temp = tempdir()?;
    let output = temp.path().join("out");
    assert!(run(&fixture(), &output, "vocab")?.status.success());
    let source = temp.path().join("corrupt.zip");
    fs::write(&source, b"not a zip")?;
    let result = run(&source, &output, "vocab")?;
    assert!(!result.status.success());
    check_golden(&output, "vocab")?;
    Ok(())
}

#[test]
fn replacement_leaves_only_complete_output() -> Result<()> {
    let temp = tempdir()?;
    for suffix in ["idx", "dat", "spx"] {
        fs::write(temp.path().join(format!("vocab.{suffix}")), b"old")?;
    }
    assert!(run(&fixture(), temp.path(), "vocab")?.status.success());
    check_golden(temp.path(), "vocab")?;
    assert_eq!(fs::read_dir(temp.path())?.count(), 3);
    Ok(())
}

#[test]
fn invalid_cli_arguments_fail_without_outputs() -> Result<()> {
    for args in [
        vec![],
        vec!["--input", "missing.zip", "--name", "../bad"],
        vec!["--input", "missing.zip", "--format", "mdict"],
    ] {
        let temp = tempdir()?;
        let result = Command::new(env!("CARGO_BIN_EXE_crossink-dict"))
            .args(&args)
            .current_dir(temp.path())
            .output()?;
        assert!(!result.status.success(), "arguments: {args:?}");
        assert_eq!(fs::read_dir(temp.path())?.count(), 0, "arguments: {args:?}");
    }
    Ok(())
}

#[test]
fn malformed_banks_fail_before_creating_output() -> Result<()> {
    for bank in [
        "{ invalid",
        "{}",
        "null",
        "[[\"embedded\\u0000nul\",\"\",\"\",\"n\",0,[\"definition\"]]]",
    ] {
        let temp = tempdir()?;
        let input = temp.path().join("input");
        let output = temp.path().join("out");
        fs::create_dir(&input)?;
        fs::write(input.join("term_bank_1.json"), bank)?;
        let result = run(&input, &output, "vocab")?;
        assert!(!result.status.success(), "bank: {bank}");
        assert!(!output.exists(), "malformed bank created output: {bank}");
    }
    Ok(())
}

#[test]
fn unrelated_json_files_do_not_count_as_banks() -> Result<()> {
    let temp = tempdir()?;
    let input = temp.path().join("input");
    fs::create_dir(&input)?;
    for name in [
        "term_bank_x.json",
        "term_bank_1.json.bak",
        "tag_bank_1.json",
    ] {
        fs::write(input.join(name), b"[]")?;
    }
    let output = temp.path().join("out");
    let result = run(&input, &output, "vocab")?;
    assert!(!result.status.success());
    assert!(String::from_utf8_lossy(&result.stderr).contains("no term_bank_N.json"));
    assert!(!output.exists());
    Ok(())
}

#[test]
fn directory_at_output_path_is_preserved() -> Result<()> {
    let temp = tempdir()?;
    fs::create_dir(temp.path().join("vocab.dat"))?;
    fs::write(temp.path().join("vocab.dat/keep"), b"user file")?;
    let result = run(&fixture(), temp.path(), "vocab")?;
    assert!(!result.status.success());
    assert_eq!(fs::read(temp.path().join("vocab.dat/keep"))?, b"user file");
    assert_eq!(fs::read_dir(temp.path())?.count(), 1);
    Ok(())
}

#[test]
fn defaults_work_outside_repository_without_metadata() -> Result<()> {
    let temp = tempdir()?;
    let input = temp.path().join("input");
    fs::create_dir(&input)?;
    fs::copy(
        fixture().join("term_bank_1.json"),
        input.join("term_bank_1.json"),
    )?;
    let result = Command::new(env!("CARGO_BIN_EXE_crossink-dict"))
        .args(["--input", "input"])
        .current_dir(temp.path())
        .output()?;
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    check_golden(&temp.path().join("output"), "vocab")?;
    Ok(())
}

#[test]
fn zip_accepts_utf8_bom_in_index_and_term_bank() -> Result<()> {
    let temp = tempdir()?;
    let source = temp.path().join("input.zip");
    let mut zip = ZipWriter::new(fs::File::create(&source)?);
    for name in ["index.json", "term_bank_1.json"] {
        zip.start_file(name, SimpleFileOptions::default())?;
        zip.write_all(b"\xef\xbb\xbf")?;
        zip.write_all(&fs::read(fixture().join(name))?)?;
    }
    zip.finish()?;
    let output = temp.path().join("out");
    let result = run(&source, &output, "vocab")?;
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    check_golden(&output, "vocab")?;
    Ok(())
}
