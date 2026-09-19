use anyhow::Result;
use std::{
    fs,
    path::Path,
    process::{Command, Output},
};
use tempfile::tempdir;

fn rebuild(path: &Path) -> Result<Output> {
    Ok(Command::new(env!("CARGO_BIN_EXE_crossink-dict"))
        .arg("--rebuild-spx")
        .arg(path)
        .output()?)
}

#[test]
fn rebuilds_all_names_without_changing_dictionary_data() -> Result<()> {
    let temp = tempdir()?;
    let golden =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../test/japanese_dict_converter/golden");
    for fixture in ["mini_jmdict", "mini_yomitan"] {
        for name in ["vocab", "names", "grammar", "jmdict", "jmnedict"] {
            for suffix in ["idx", "dat"] {
                fs::copy(
                    golden.join(fixture).join(format!("vocab.{suffix}")),
                    temp.path().join(format!("{name}.{suffix}")),
                )?;
            }
            fs::write(temp.path().join(format!("{name}.spx")), b"stale")?;
        }
        let result = rebuild(temp.path())?;
        assert!(
            result.status.success(),
            "{}",
            String::from_utf8_lossy(&result.stderr)
        );
        for name in ["vocab", "names", "grammar", "jmdict", "jmnedict"] {
            for suffix in ["idx", "dat", "spx"] {
                assert_eq!(
                    fs::read(temp.path().join(format!("{name}.{suffix}")))?,
                    fs::read(golden.join(fixture).join(format!("vocab.{suffix}")))?
                );
            }
        }
        assert_eq!(fs::read_dir(temp.path())?.count(), 15);
    }
    Ok(())
}

#[test]
fn rebuilds_three_checkpoints_without_a_data_file() -> Result<()> {
    let temp = tempdir()?;
    let mut idx = Vec::new();
    for i in 0..97 {
        let mut record = [0; 40];
        record[..3].copy_from_slice(format!("{i:03}").as_bytes());
        idx.extend_from_slice(&record);
    }
    fs::write(temp.path().join("vocab.idx"), &idx)?;
    let result = rebuild(temp.path())?;
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let spx = fs::read(temp.path().join("vocab.spx"))?;
    assert_eq!(spx.len(), 128);
    assert_eq!(
        &spx[..32],
        b"CPSPX1\0\0\x01\0\0\0\x30\0\0\0\x61\0\0\0\x03\0\0\0\0\0\0\0\0\0\0\0"
    );
    for (checkpoint, record) in [0, 48, 96].into_iter().enumerate() {
        assert_eq!(
            &spx[32 + checkpoint * 32..64 + checkpoint * 32],
            &idx[record * 40..record * 40 + 32]
        );
    }
    Ok(())
}

#[test]
fn failures_preserve_existing_sidecar_and_stale_temporary_file() -> Result<()> {
    let temp = tempdir()?;
    fs::write(temp.path().join("vocab.spx"), b"original")?;
    fs::write(temp.path().join("vocab.idx"), [0; 39])?;
    let result = rebuild(temp.path())?;
    assert!(!result.status.success(), "partial record must fail");
    assert!(
        String::from_utf8_lossy(&result.stderr).contains("Invalid index"),
        "must reject malformed index"
    );
    assert_eq!(fs::read(temp.path().join("vocab.spx"))?, b"original");
    fs::write(temp.path().join("vocab.idx"), [0; 40])?;
    fs::write(temp.path().join("vocab.spx.tmp"), b"recovery")?;
    let result = rebuild(temp.path())?;
    assert!(!result.status.success(), "stale temporary file must fail");
    assert!(
        String::from_utf8_lossy(&result.stderr).contains("Cannot create"),
        "must reject existing temporary file"
    );
    assert_eq!(fs::read(temp.path().join("vocab.spx.tmp"))?, b"recovery");
    assert_eq!(fs::read(temp.path().join("vocab.spx"))?, b"original");
    Ok(())
}

#[test]
fn cli_supports_help_and_skips_missing_indexes_but_rejects_mixed_modes() -> Result<()> {
    let temp = tempdir()?;
    let result = rebuild(temp.path())?;
    assert!(
        result.status.success(),
        "empty dictionary directory should be skipped"
    );
    assert!(
        String::from_utf8_lossy(&result.stdout).contains("skip vocab"),
        "missing indexes should be reported"
    );
    for flag in ["-h", "--help"] {
        let result = Command::new(env!("CARGO_BIN_EXE_crossink-dict"))
            .arg(flag)
            .output()?;
        assert!(result.status.success(), "help must succeed");
        assert!(
            String::from_utf8_lossy(&result.stdout).contains("--rebuild-spx"),
            "help must document rebuilding"
        );
    }
    for extra in [
        vec![],
        vec!["--rebuild-spx"],
        vec!["--rebuild-spx", ".", "--input", "input.zip"],
        vec!["--rebuild-spx", ".", "--name", "names"],
    ] {
        assert!(
            !Command::new(env!("CARGO_BIN_EXE_crossink-dict"))
                .args(extra)
                .output()?
                .status
                .success(),
            "invalid arguments must fail"
        );
    }
    Ok(())
}

#[test]
fn failed_publication_cleans_temporary_file_and_preserves_destination() -> Result<()> {
    let temp = tempdir()?;
    fs::write(temp.path().join("vocab.idx"), [])?;
    let destination = temp.path().join("vocab.spx");
    fs::create_dir(&destination)?;
    fs::write(destination.join("keep"), b"user data")?;
    let result = rebuild(temp.path())?;
    assert!(!result.status.success(), "directory must not be replaced");
    assert!(
        String::from_utf8_lossy(&result.stderr).contains("Cannot publish"),
        "must reach publication failure"
    );
    assert_eq!(fs::read(destination.join("keep"))?, b"user data");
    assert!(
        !temp.path().join("vocab.spx.tmp").exists(),
        "temporary file must be cleaned up"
    );
    Ok(())
}
