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
    LegacyConvert(crossink_manga::legacy::Options),
    Compare {
        reference: PathBuf,
        candidate: PathBuf,
        #[arg(long)]
        report: PathBuf,
    },
    Convert(ConvertArgs),
    Prepare(ConvertArgs),
    Validate {
        folder: PathBuf,
    },
}

#[derive(Args)]
struct ConvertArgs {
    #[arg(long, value_enum, default_value = "native")]
    backend: crossink_manga::native::Backend,
    #[arg(long)]
    models: Option<PathBuf>,
    input: PathBuf,
    #[arg(short, long)]
    output: PathBuf,
    #[arg(long, value_enum, default_value = "x4")]
    device: Device,
    #[arg(long, value_enum, default_value = "floyd-steinberg")]
    dither: crossink_manga::format::Dither,
    #[arg(long)]
    panel_map: Option<PathBuf>,
    #[arg(long)]
    mokuro: Option<PathBuf>,
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
            dither: self.dither,
            mokuro: self.mokuro.clone(),
            panel_map: self.panel_map.clone(),
            work: self.work.clone(),
            title: self.title.clone(),
        }
    }
}

fn run() -> anyhow::Result<()> {
    match Cli::parse().command {
        Command::LegacyConvert(options) => crossink_manga::legacy::convert(&options),
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

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    #[test]
    fn python_runtime_options_are_rejected() {
        for options in [["--backend", "upstream"], ["--uv", "/tmp/uv"]] {
            assert!(
                Cli::try_parse_from([
                    "crossink-manga",
                    "convert",
                    "source.cbz",
                    "--output",
                    "book",
                    options[0],
                    options[1],
                ])
                .is_err()
            );
        }
    }
    #[test]
    fn dither_cli_defaults_and_override() {
        let parse = |extra: &[&str]| {
            let mut args = vec![
                "crossink-manga",
                "convert",
                "source.cbz",
                "--output",
                "book",
            ];
            args.extend_from_slice(extra);
            Cli::try_parse_from(args)
        };
        for (args, expected) in [
            (&[][..], crossink_manga::format::Dither::FloydSteinberg),
            (
                &["--dither", "bayer"][..],
                crossink_manga::format::Dither::Bayer,
            ),
        ] {
            let cli = parse(args).unwrap();
            let Command::Convert(convert) = cli.command else {
                panic!("wrong command");
            };
            assert_eq!(convert.options().dither, expected);
        }
        assert!(parse(&["--dither", "unknown"]).is_err());
    }
}
