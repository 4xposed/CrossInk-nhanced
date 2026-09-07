use anyhow::{Context, Result, bail};
use std::env;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

const MOKURO_VERSION: &str = "0.2.5";
const PYTHON_VERSION: &str = "3.11";
const ERROR_LOG_TAIL_BYTES: u64 = 16 * 1024;

/// Runs pinned Mokuro through a managed `uv` tool environment and returns the
/// generated `.mokuro` JSON path.
///
/// `uv_override` takes precedence over a `uv` binary beside this executable and
/// one found on `PATH`. The subprocess always uses CPU inference, disables its
/// confirmation prompt, and disables legacy HTML output.
///
/// # Errors
///
/// Returns an error when the crop directory or runtime is missing, the process
/// cannot start or exits unsuccessfully, or the expected OCR JSON is absent.
pub fn run_mokuro(crops: &Path, uv_override: Option<&Path>) -> Result<PathBuf> {
    if !crops.is_dir() {
        bail!("crop directory does not exist: {}", crops.display());
    }
    let crops = std::path::absolute(crops)
        .with_context(|| format!("resolve crop directory {}", crops.display()))?;
    let uv = locate_uv(uv_override)?;
    let output_path = crops.with_extension("mokuro");
    let log_path = crops
        .parent()
        .context("crop directory has no parent for OCR log")?
        .join("ocr.log");
    let log = File::create(&log_path)
        .with_context(|| format!("create OCR log {}", log_path.display()))?;
    let stdout_log = log
        .try_clone()
        .with_context(|| format!("open OCR stdout log {}", log_path.display()))?;
    eprintln!("Running Mokuro OCR; progress log: {}", log_path.display());
    let status = Command::new(&uv)
        .arg("tool")
        .arg("run")
        .arg("--python")
        .arg(PYTHON_VERSION)
        .arg("--from")
        .arg(format!("mokuro=={MOKURO_VERSION}"))
        .arg("mokuro")
        .arg(&crops)
        .arg("--force_cpu=True")
        .arg("--disable_confirmation=True")
        .arg("--legacy_html=False")
        .stdout(Stdio::from(stdout_log))
        .stderr(Stdio::from(log))
        .status()
        .with_context(|| format!("failed to start managed Mokuro runtime {}", uv.display()))?;

    if !status.success() {
        let tail = read_log_tail(&log_path).unwrap_or_else(|error| {
            format!("could not read OCR log {}: {error}", log_path.display())
        });
        bail!(
            "managed Mokuro runtime failed with status {status}; tail of {}:\n{}",
            log_path.display(),
            tail.trim()
        );
    }
    if !output_path.is_file() {
        bail!(
            "Mokuro reported success but OCR output was not created: {}",
            output_path.display()
        );
    }
    Ok(output_path)
}

fn read_log_tail(path: &Path) -> Result<String> {
    let mut file = File::open(path).with_context(|| format!("open OCR log {}", path.display()))?;
    let length = file
        .metadata()
        .with_context(|| format!("inspect OCR log {}", path.display()))?
        .len();
    file.seek(SeekFrom::Start(length.saturating_sub(ERROR_LOG_TAIL_BYTES)))
        .with_context(|| format!("seek OCR log {}", path.display()))?;
    let mut bytes = Vec::with_capacity(
        usize::try_from(length.min(ERROR_LOG_TAIL_BYTES)).context("OCR log tail is too large")?,
    );
    file.take(ERROR_LOG_TAIL_BYTES)
        .read_to_end(&mut bytes)
        .with_context(|| format!("read OCR log {}", path.display()))?;
    Ok(String::from_utf8_lossy(&bytes).into_owned())
}

fn locate_uv(uv_override: Option<&Path>) -> Result<PathBuf> {
    if let Some(path) = uv_override {
        if !path.is_file() {
            bail!(
                "uv runtime override does not exist or is not a file: {}",
                path.display()
            );
        }
        return Ok(path.to_path_buf());
    }

    let executable_name = if cfg!(windows) { "uv.exe" } else { "uv" };
    if let Ok(current_executable) = env::current_exe()
        && let Some(directory) = current_executable.parent()
    {
        let bundled = directory.join(executable_name);
        if bundled.is_file() {
            return Ok(bundled);
        }
    }
    if let Some(path) = env::var_os("PATH") {
        for directory in env::split_paths(&path) {
            let candidate = directory.join(executable_name);
            if candidate.is_file() {
                return Ok(candidate);
            }
        }
    }
    bail!("managed Mokuro runtime is unavailable: no bundled or PATH uv executable was found")
}

#[cfg(test)]
mod tests {
    use super::run_mokuro;
    use std::fs;
    use std::path::Path;
    use tempfile::TempDir;

    #[test]
    fn missing_explicit_runtime_should_return_clear_error() {
        let fixture = TempDir::new().expect("fixture directory");
        let crops = fixture.path().join("crops");
        fs::create_dir(&crops).expect("crops");

        let error = run_mokuro(&crops, Some(&fixture.path().join("missing-uv")))
            .expect_err("missing runtime");

        assert!(error.to_string().contains("uv runtime override"));
    }

    #[cfg(unix)]
    #[test]
    fn managed_runtime_should_use_pinned_mokuro_and_required_options() {
        use std::os::unix::fs::PermissionsExt;

        let fixture = TempDir::new().expect("fixture directory");
        let crops = fixture.path().join("crops");
        let fake_uv = fixture.path().join("uv");
        let args_file = fixture.path().join("uv-args");
        fs::create_dir(&crops).expect("crops");
        let script = format!(
            "#!/bin/sh\nprintf '%s\\n' \"$@\" > '{}'\necho ocr-progress\ntouch '{}.mokuro'\n",
            args_file.display(),
            crops.display()
        );
        fs::write(&fake_uv, script).expect("fake uv");
        let mut permissions = fs::metadata(&fake_uv).expect("metadata").permissions();
        permissions.set_mode(0o755);
        fs::set_permissions(&fake_uv, permissions).expect("make executable");

        let result = run_mokuro(&crops, Some(&fake_uv)).expect("run fake uv");

        assert_eq!(result, crops.with_extension("mokuro"));
        assert_eq!(
            fs::read_to_string(args_file).expect("recorded args"),
            format!(
                "tool\nrun\n--python\n3.11\n--from\nmokuro==0.2.5\nmokuro\n{}\n--force_cpu=True\n--disable_confirmation=True\n--legacy_html=False\n",
                crops.display()
            )
        );
        assert_eq!(
            fs::read_to_string(fixture.path().join("ocr.log")).expect("OCR log"),
            "ocr-progress\n"
        );
    }

    #[cfg(unix)]
    #[test]
    fn successful_process_without_json_should_report_ocr_output_failure() {
        use std::os::unix::fs::PermissionsExt;

        let fixture = TempDir::new().expect("fixture directory");
        let crops = fixture.path().join("crops");
        let fake_uv = fixture.path().join("uv");
        fs::create_dir(&crops).expect("crops");
        fs::write(&fake_uv, "#!/bin/sh\nexit 0\n").expect("fake uv");
        let mut permissions = fs::metadata(&fake_uv).expect("metadata").permissions();
        permissions.set_mode(0o755);
        fs::set_permissions(&fake_uv, permissions).expect("make executable");

        let error = run_mokuro(&crops, Some(&fake_uv)).expect_err("missing output");

        assert!(error.to_string().contains("OCR output"));
    }

    #[cfg(unix)]
    #[test]
    fn failed_process_should_include_runtime_diagnostics() {
        use std::os::unix::fs::PermissionsExt;

        let fixture = TempDir::new().expect("fixture directory");
        let crops = fixture.path().join("crops");
        let fake_uv = fixture.path().join("uv");
        fs::create_dir(&crops).expect("crops");
        fs::write(&fake_uv, "#!/bin/sh\necho model-failed >&2\nexit 7\n").expect("fake uv");
        let mut permissions = fs::metadata(&fake_uv).expect("metadata").permissions();
        permissions.set_mode(0o755);
        fs::set_permissions(&fake_uv, permissions).expect("make executable");

        let error = run_mokuro(&crops, Some(&fake_uv)).expect_err("runtime failure");

        assert!(error.to_string().contains("model-failed"));
    }

    #[test]
    fn missing_crop_directory_should_fail_before_runtime_start() {
        let fixture = TempDir::new().expect("fixture directory");
        let fake_uv = fixture
            .path()
            .join(if cfg!(windows) { "uv.exe" } else { "uv" });

        let error =
            run_mokuro(Path::new("missing-crops"), Some(&fake_uv)).expect_err("missing crops");

        assert!(error.to_string().contains("crop directory"));
    }
}
