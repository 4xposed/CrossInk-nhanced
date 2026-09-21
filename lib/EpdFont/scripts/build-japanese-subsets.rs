//! Rebuild the two Japanese fallback headers using the pinned font inputs.
//! Run with `cargo run --manifest-path lib/EpdFont/scripts/Cargo.toml --bin build-japanese-subsets -- [--source FONT]`.
//! The upstream fontconvert requires Python; source preparation uses native HarfBuzz.

use sha2::{Digest, Sha256};
use std::env;
use std::error::Error;
use std::ffi::OsString;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode, Output};

type Result<T> = std::result::Result<T, Box<dyn Error>>;

fn intervals(text: &str) -> Result<Vec<(u32, u32)>> {
    let mut points = Vec::new();
    for (index, line) in text.lines().enumerate() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let point = u32::from_str_radix(line.trim_start_matches("0x"), 16)
            .map_err(|error| format!("Invalid codepoint on line {}: {error}", index + 1))?;
        if char::from_u32(point).is_none() {
            return Err(format!("Invalid Unicode codepoint on line {}: {line}", index + 1).into());
        }
        points.push(point);
    }
    if points.is_empty() {
        return Err("The codepoint list is empty".into());
    }
    points.sort_unstable();
    points.dedup();
    let mut ranges: Vec<(u32, u32)> = Vec::new();
    for point in points {
        if let Some((_, last)) = ranges.last_mut() {
            if point == *last + 1 {
                *last = point;
                continue;
            }
        }
        ranges.push((point, point));
    }
    Ok(ranges)
}

fn checked_output(command: &mut Command, description: &str) -> Result<Output> {
    let output = command
        .output()
        .map_err(|error| format!("Could not run {description}: {error}"))?;
    if !output.status.success() {
        return Err(format!(
            "{description} failed ({}):\n{}",
            output.status,
            String::from_utf8_lossy(&output.stderr)
        )
        .into());
    }
    Ok(output)
}

// HarfBuzz performs native variable-font instancing and subsetting. Validate the
// generated face before replacing a pinned input: a failed subprocess must not
// leave an empty or incomplete font behind.
fn subset_font(
    source: &Path,
    output: &Path,
    ranges: &[(u32, u32)],
    harfbuzz: &OsString,
) -> Result<()> {
    let input = fs::read(source)?;
    let face = ttf_parser::Face::parse(&input, 0)?;
    let variable_weight = face
        .variation_axes()
        .into_iter()
        .any(|axis| axis.tag == ttf_parser::Tag::from_bytes(b"wght"));
    let temporary = output.with_extension("subset.tmp");
    let unicodes = ranges
        .iter()
        .map(|(a, b)| format!("{a:X}-{b:X}"))
        .collect::<Vec<_>>()
        .join(",");
    let mut command = Command::new(harfbuzz);
    command
        .arg(source)
        .arg(format!("--unicodes={unicodes}"))
        .args(["--name-IDs=*", "--name-languages=*", "--name-legacy"]);
    if variable_weight {
        command.arg("--variations=wght=400");
    }
    command.arg("--output-file").arg(&temporary);
    let result = (|| -> Result<()> {
        checked_output(
            &mut command,
            "HarfBuzz subset generation (install hb-subset or set HB_SUBSET)",
        )?;
        let bytes = fs::read(&temporary)?;
        let face = ttf_parser::Face::parse(&bytes, 0)?;
        if face
            .variation_axes()
            .into_iter()
            .any(|axis| axis.tag == ttf_parser::Tag::from_bytes(b"wght"))
        {
            return Err("HarfBuzz did not pin the weight axis".into());
        }
        for &(first, last) in ranges {
            for point in first..=last {
                let character = char::from_u32(point).ok_or("Invalid Unicode coverage")?;
                if face.glyph_index(character).is_none() {
                    return Err(format!("Subset lacks U+{point:04X}").into());
                }
            }
        }
        fs::rename(&temporary, output)?;
        Ok(())
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    result
}

fn generate(here: &Path, python: &OsString, source: Option<&Path>) -> Result<()> {
    let fonts = here.join("../builtinFonts");
    let pinned = fonts.join("source/NotoSansJP");
    let coverage = pinned.join("codepoints.txt");
    let text = fs::read_to_string(&coverage)
        .map_err(|error| format!("Could not read {}: {error}", coverage.display()))?;
    let ranges = intervals(&text)?;
    if let Some(source) = source {
        let source_hash = format!("{:x}\n", Sha256::digest(fs::read(source)?));
        let harfbuzz = env::var_os("HB_SUBSET").unwrap_or_else(|| "hb-subset".into());
        subset_font(
            source,
            &pinned.join("NotoSansJP-Regular.ttf"),
            &ranges,
            &harfbuzz,
        )?;
        fs::write(pinned.join("source-sha256.txt"), source_hash)?;
    }
    for size in [8, 12] {
        let name = format!("notosansjp_joyo_{size}_regular");
        let mut command = Command::new(python);
        command.current_dir(here).args([
            "fontconvert.py",
            &name,
            &size.to_string(),
            "../builtinFonts/source/NotoSansJP/NotoSansJP-Regular.ttf",
            "--2bit",
            "--compress",
            "--no-default-intervals",
        ]);
        for (first, last) in &ranges {
            command.args(["--additional-intervals", &format!("0x{first:X},0x{last:X}")]);
        }
        let output = checked_output(&mut command, &format!("fontconvert ({size} pt)"))?;
        let target = fonts.join(format!("{name}.h"));
        let temporary = target.with_extension("tmp");
        fs::write(&temporary, output.stdout)
            .map_err(|error| format!("Could not write {}: {error}", temporary.display()))?;
        fs::rename(&temporary, &target)
            .map_err(|error| format!("Could not replace {}: {error}", target.display()))?;
        println!("Generated {name}.h");
    }
    Ok(())
}

fn run() -> Result<()> {
    let mut source: Option<PathBuf> = None;
    let mut args = env::args_os().skip(1);
    while let Some(arg) = args.next() {
        if arg == "--help" || arg == "-h" {
            println!("Rebuild the firmware's two Japanese fallback headers from pinned font inputs.\n\nUsage: build-japanese-subsets [--source FONT]\n\n  --source FONT  Replace the pinned source with a regular-weight subset of FONT\n\nSet PYTHON for upstream fontconvert (default: python3). Source preparation requires native hb-subset (override with HB_SUBSET).");
            return Ok(());
        } else if arg == "--source" {
            source = Some(args.next().ok_or("--source requires a font path")?.into());
        } else if let Some(value) = arg.to_str().and_then(|s| s.strip_prefix("--source=")) {
            if value.is_empty() {
                return Err("--source requires a font path".into());
            }
            source = Some(value.into());
        } else {
            return Err(format!("Unknown argument: {} (use --help)", arg.to_string_lossy()).into());
        }
    }
    let python = env::var_os("PYTHON").unwrap_or_else(|| "python3".into());
    generate(
        Path::new(env!("CARGO_MANIFEST_DIR")),
        &python,
        source.as_deref(),
    )
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("build-japanese-subsets: {error}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn native_subset_preserves_requested_glyphs() {
        if Command::new("hb-subset").arg("--version").output().is_err() {
            eprintln!("hb-subset not installed; native integration check unavailable");
            return;
        }
        let source = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../builtinFonts/source/NotoSansJP/NotoSansJP-Regular.ttf");
        let directory = env::temp_dir().join(format!("crossink-subset-{}", std::process::id()));
        fs::create_dir_all(&directory).unwrap();
        let output = directory.join("subset.ttf");
        subset_font(
            &source,
            &output,
            &[(0x41, 0x42), (0x732B, 0x732B)],
            &OsString::from("hb-subset"),
        )
        .unwrap();
        let bytes = fs::read(&output).unwrap();
        assert!(!bytes.is_empty());
        assert!(bytes.len() < fs::metadata(&source).unwrap().len() as usize);
        fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn sorts_deduplicates_and_merges_only_adjacent_codepoints() {
        assert_eq!(
            intervals("# coverage\n732B\n0042\n0041\n0042\n0044\n\n").unwrap(),
            vec![(0x41, 0x42), (0x44, 0x44), (0x732B, 0x732B)]
        );
    }

    #[test]
    fn rejects_invalid_hex_with_line_number() {
        let error = intervals("0041\ninvalid\n").unwrap_err().to_string();
        assert!(error.contains("line 2"), "{error}");
    }

    #[test]
    fn rejects_non_unicode_codepoints() {
        for input in ["110000", "D800"] {
            assert!(intervals(input).is_err(), "accepted {input}");
        }
    }

    #[test]
    fn rejects_empty_coverage() {
        assert!(intervals("# no glyphs\n\n").is_err());
    }
}
