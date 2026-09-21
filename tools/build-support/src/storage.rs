use anyhow::{Context, Result, bail, ensure};
use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    fs,
    path::{Component, Path},
    process::Command,
};

#[derive(Deserialize)]
struct Manifest {
    revision: String,
    files: BTreeMap<String, Hashes>,
}

#[derive(Deserialize)]
struct Hashes {
    before: String,
    after: String,
}

fn git(package: &Path, arguments: &[&std::ffi::OsStr]) -> Result<String> {
    let output = Command::new("git")
        .arg("-C")
        .arg(package)
        .args(arguments)
        .output()
        .context("could not run git for simulator storage patch")?;
    ensure!(
        output.status.success(),
        "simulator storage git operation failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    String::from_utf8(output.stdout).context("git output is not UTF-8")
}

pub fn apply(project: &Path, package: &Path) -> Result<&'static str> {
    ensure!(
        !package.is_symlink() && package.join("library.json").is_file(),
        "actual pinned simulator dependency missing; custom symlink overrides unsupported"
    );
    let revision = git(package, &["rev-parse".as_ref(), "HEAD".as_ref()])?;
    apply_checked(project, package, revision.trim())
}

fn digest(package: &Path, name: &str) -> Result<String> {
    let relative = Path::new(name);
    ensure!(
        relative
            .components()
            .all(|part| matches!(part, Component::Normal(_))),
        "invalid patch manifest path {name}"
    );
    let mut path = package.to_path_buf();
    for part in relative.components() {
        path.push(part);
        ensure!(
            !path.is_symlink(),
            "symlink patch preimage {}",
            path.display()
        );
    }
    let bytes =
        fs::read(&path).with_context(|| format!("read patch preimage {}", path.display()))?;
    Ok(format!("{:x}", Sha256::digest(bytes)))
}

fn apply_checked(project: &Path, package: &Path, revision: &str) -> Result<&'static str> {
    let directory = project.join("tools/simulator-patches");
    let manifest: Manifest =
        serde_json::from_slice(&fs::read(directory.join("checked-storage.json"))?)?;
    ensure!(
        revision == manifest.revision,
        "unrecognized dependency revision; preserve checkout and inspect"
    );
    ensure!(!manifest.files.is_empty(), "empty storage patch manifest");
    let mut all_before = true;
    let mut all_after = true;
    for (name, hashes) in &manifest.files {
        let actual = digest(package, name)?;
        all_before &= actual == hashes.before;
        all_after &= actual == hashes.after;
    }
    if all_after {
        return Ok("already applied");
    }
    if !all_before {
        bail!("unknown or mixed pre/post images; no files changed");
    }
    let patch = fs::canonicalize(directory.join("checked-storage.patch"))?;
    git(
        package,
        &["apply".as_ref(), "--check".as_ref(), patch.as_os_str()],
    )?;
    git(package, &["apply".as_ref(), patch.as_os_str()])?;
    for (name, hashes) in &manifest.files {
        ensure!(
            digest(package, name)? == hashes.after,
            "postimage verification failed for {name}"
        );
    }
    Ok("applied")
}

#[cfg(test)]
mod tests {
    use super::*;
    use sha2::{Digest, Sha256};
    use std::fs;

    fn fixture() -> tempfile::TempDir {
        let root = tempfile::tempdir().unwrap();
        fs::create_dir_all(root.path().join("tools/simulator-patches")).unwrap();
        fs::create_dir_all(root.path().join("package/src")).unwrap();
        fs::write(root.path().join("package/library.json"), "{}").unwrap();
        fs::write(root.path().join("package/src/a"), "before\n").unwrap();
        fs::write(root.path().join("package/src/b"), "before\n").unwrap();
        let before = format!("{:x}", Sha256::digest(b"before\n"));
        let after = format!("{:x}", Sha256::digest(b"after\n"));
        // No test creates Git commits: these fixtures exercise the checked
        // mutation separately from the read-only revision query.
        let manifest = serde_json::json!({"revision":"pinned", "files":{
            "src/a":{"before":before,"after":after},
            "src/b":{"before":before,"after":after}}});
        fs::write(
            root.path()
                .join("tools/simulator-patches/checked-storage.json"),
            manifest.to_string(),
        )
        .unwrap();
        fs::write(root.path().join("tools/simulator-patches/checked-storage.patch"),
            "--- a/src/a\n+++ b/src/a\n@@ -1 +1 @@\n-before\n+after\n--- a/src/b\n+++ b/src/b\n@@ -1 +1 @@\n-before\n+after\n").unwrap();
        root
    }

    #[test]
    fn applies_once_and_accepts_verified_postimages() {
        let root = fixture();
        let package = root.path().join("package");
        assert_eq!(
            apply_checked(root.path(), &package, "pinned").unwrap(),
            "applied"
        );
        assert_eq!(fs::read(package.join("src/a")).unwrap(), b"after\n");
        assert_eq!(
            apply_checked(root.path(), &package, "pinned").unwrap(),
            "already applied"
        );
    }

    #[test]
    fn rejects_unknown_revision_without_modification() {
        let root = fixture();
        let package = root.path().join("package");
        assert!(apply_checked(root.path(), &package, "unknown").is_err());
        assert_eq!(fs::read(package.join("src/a")).unwrap(), b"before\n");
    }

    #[test]
    fn refuses_unknown_or_mixed_files_without_touching_other_files() {
        for contents in ["user edit\n", "after\n"] {
            let root = fixture();
            let package = root.path().join("package");
            fs::write(package.join("src/a"), contents).unwrap();
            assert!(apply_checked(root.path(), &package, "pinned").is_err());
            assert_eq!(
                fs::read(package.join("src/a")).unwrap(),
                contents.as_bytes()
            );
            assert_eq!(fs::read(package.join("src/b")).unwrap(), b"before\n");
        }
    }

    #[test]
    fn checks_whole_patch_before_modifying_any_file() {
        let root = fixture();
        let patch = root
            .path()
            .join("tools/simulator-patches/checked-storage.patch");
        let invalid = fs::read_to_string(&patch)
            .unwrap()
            .replace("--- a/src/b", "--- a/src/missing")
            .replace("+++ b/src/b", "+++ b/src/missing");
        fs::write(patch, invalid).unwrap();
        let package = root.path().join("package");
        assert!(apply_checked(root.path(), &package, "pinned").is_err());
        assert_eq!(fs::read(package.join("src/a")).unwrap(), b"before\n");
    }

    #[test]
    fn cli_reports_missing_dependency() {
        // Exercise the public entry point's filesystem validation.
        let root = fixture();
        assert!(apply(root.path(), &root.path().join("missing")).is_err());
    }
}
