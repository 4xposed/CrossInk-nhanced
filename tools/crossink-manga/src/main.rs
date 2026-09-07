use clap::{Args, Parser, Subcommand};
use crossink_manga::{compare, device::Device, mokuro, pipeline, validate};
use std::{path::PathBuf, process::ExitCode};

#[derive(Parser)]
#[command(
    version,
    about = "Convert manga archives into panel-first books for CrossInk"
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Compare complete OCR outputs by region overlap and Unicode character edits.
    Compare {
        reference: PathBuf,
        candidate: PathBuf,
        #[arg(long)]
        report: PathBuf,
    },
    /// Extract panels, recognize original-resolution text, and export a device-sized book.
    Convert(ConvertArgs),
    /// Prepare lossless panel crops and numbered previews for review before OCR.
    Prepare(ConvertArgs),
    /// Check the images and indexed text of an exported book.
    Validate { folder: PathBuf },
}

#[derive(Args)]
struct ConvertArgs {
    /// Native Rust inference, or upstream Python Mokuro for comparison.
    #[arg(long, value_enum, default_value = "native")]
    backend: crossink_manga::native::Backend,
    /// Native model bundle directory; defaults to models beside the executable.
    #[arg(long)]
    models: Option<PathBuf>,
    /// CBZ/ZIP, CBR/RAR, or a directory of manga images.
    input: PathBuf,
    #[arg(short, long)]
    output: PathBuf,
    #[arg(long, value_enum, default_value = "x4")]
    device: Device,
    /// Ordered source-coordinate rectangles, keyed by relative source filename.
    #[arg(long)]
    panel_map: Option<PathBuf>,
    /// Reuse OCR of the prepared panel crops (not OCR of the original pages).
    #[arg(long)]
    mokuro: Option<PathBuf>,
    /// Override the bundled/PATH uv executable.
    #[arg(long)]
    uv: Option<PathBuf>,
    /// Persistent OCR work folder; defaults to OUTPUT.work.
    #[arg(long)]
    work: Option<PathBuf>,
    #[arg(long)]
    title: Option<String>,
}

impl ConvertArgs {
    fn options(&self) -> pipeline::Options {
        pipeline::Options {
            backend: self.backend,
            models: self.models.clone(),
            device: self.device,
            mokuro: self.mokuro.clone(),
            panel_map: self.panel_map.clone(),
            uv: self.uv.clone(),
            work: self.work.clone(),
            title: self.title.clone(),
        }
    }
}

fn run() -> anyhow::Result<()> {
    match Cli::parse().command {
        Command::Compare {
            reference,
            candidate,
            report,
        } => {
            let reference: mokuro::Volume = serde_json::from_slice(&std::fs::read(reference)?)?;
            let candidate: mokuro::Volume = serde_json::from_slice(&std::fs::read(candidate)?)?;
            let result = compare::compare(&reference, &candidate)?;
            std::fs::write(&report, serde_json::to_vec_pretty(&result)?)?;
            println!(
                "{} panels: recall {:.2}%, precision {:.2}%, matched text disagreement {:.2}%, panel text disagreement {:.2}%",
                result.pages,
                result.region_recall * 100.0,
                result.region_precision * 100.0,
                result.matched_character_error_rate * 100.0,
                result.panel_character_error_rate * 100.0
            );
            anyhow::ensure!(
                result.comparable,
                "comparison thresholds not met; see {}",
                report.display()
            );
            Ok(())
        }
        Command::Convert(args) => pipeline::convert(&args.input, &args.output, &args.options()),
        Command::Prepare(args) => {
            let work = pipeline::prepare(&args.input, &args.output, &args.options())?;
            println!("Prepared crops and previews: {}", work.display());
            Ok(())
        }
        Command::Validate { folder } => {
            validate::validate(&folder)?;
            println!("Valid manga book: {}", folder.display());
            Ok(())
        }
    }
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("Error: {error:#}");
            ExitCode::FAILURE
        }
    }
}
