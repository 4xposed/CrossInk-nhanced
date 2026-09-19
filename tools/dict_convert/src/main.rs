mod binary;
mod content;
mod yomitan;

use clap::Parser;
use std::{path::PathBuf, process::ExitCode};

#[derive(Parser)]
#[command(
    version,
    about = "Convert Yomitan dictionaries or rebuild CrossInk sparse indexes"
)]
struct Args {
    #[arg(long, required_unless_present = "rebuild_spx")]
    input: Option<PathBuf>,
    /// Rebuild .spx files from existing .idx files in this directory.
    #[arg(long, value_name = "DIRECTORY", conflicts_with_all = ["input", "output_dir", "name", "format"])]
    rebuild_spx: Option<PathBuf>,
    #[arg(long, default_value = "output")]
    output_dir: PathBuf,
    #[arg(long, default_value = "vocab", value_parser = ["vocab", "names", "grammar"])]
    name: String,
    #[arg(long, value_parser = ["yomitan"])]
    format: Option<String>,
}

#[expect(
    clippy::print_stderr,
    reason = "CLI failures are reported to the user on stderr."
)]
fn main() -> ExitCode {
    let args = Args::parse();
    let result = if let Some(directory) = args.rebuild_spx {
        binary::rebuild_sparse_indexes(&directory)
    } else if let Some(input) = args.input {
        yomitan::convert(&input, &args.output_dir, &args.name)
    } else {
        Err(anyhow::anyhow!(
            "An input or sparse-index directory is required"
        ))
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("ERROR: {error:#}");
            ExitCode::FAILURE
        }
    }
}
