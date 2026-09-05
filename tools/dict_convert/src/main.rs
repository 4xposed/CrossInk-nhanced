mod binary;
mod content;
mod yomitan;

use clap::Parser;
use std::{path::PathBuf, process::ExitCode};

#[derive(Parser)]
#[command(
    version,
    about = "Convert a Yomitan ZIP or directory to CrossInk dictionary files"
)]
struct Args {
    #[arg(long)]
    input: PathBuf,
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
    match yomitan::convert(&args.input, &args.output_dir, &args.name) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("ERROR: {error:#}");
            ExitCode::FAILURE
        }
    }
}
