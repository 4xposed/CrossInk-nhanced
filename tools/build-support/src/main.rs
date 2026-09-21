use anyhow::{Result, bail};
use std::{env, path::Path, process::ExitCode};

mod storage;

fn run() -> Result<()> {
    let args: Vec<_> = env::args_os().skip(1).collect();
    if args.len() != 3 || args[0] != "patch-storage" {
        bail!("usage: crossink-build-support patch-storage PROJECT SIMULATOR_PACKAGE");
    }
    println!(
        "Simulator checked storage: {}",
        storage::apply(Path::new(&args[1]), Path::new(&args[2]))?
    );
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("crossink-build-support: {error:#}");
            ExitCode::FAILURE
        }
    }
}
