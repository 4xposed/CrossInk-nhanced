// SPDX-License-Identifier: MIT
#[path = "../../src/input.rs"]
mod input;

use anyhow::{Context, Result, bail};
use std::{path::PathBuf, process::ExitCode};

fn run() -> Result<()> {
    let mut args = std::env::args_os().skip(1);
    let source = PathBuf::from(args.next().context("usage: crossink-rar INPUT DEST")?);
    let destination = PathBuf::from(args.next().context("usage: crossink-rar INPUT DEST")?);
    if args.next().is_some() {
        bail!("usage: crossink-rar INPUT DEST");
    }
    let extension = source.extension().and_then(|s| s.to_str()).unwrap_or("");
    if !extension.eq_ignore_ascii_case("rar") && !extension.eq_ignore_ascii_case("cbr") {
        bail!("crossink-rar accepts only RAR/CBR archives");
    }
    if !std::fs::symlink_metadata(&source)?.is_file() {
        bail!("RAR input must be a regular archive file");
    }
    input::collect(&source, &destination)?;
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("RAR extraction failed: {error:#}");
            ExitCode::FAILURE
        }
    }
}
