//! The `eventtrace` command: tools that read EventTrace traces, each given the design's manifest.

use std::io::{self, BufWriter, ErrorKind, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use eventtrace::{Error, Events, Manifest, Result, text};

#[derive(Parser)]
#[command(name = "eventtrace", about = "Reads EventTrace traces")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Prints a trace as text, one line per event, with absent fields left out.
    Dump {
        /// The design's manifest, <long name>.eventtrace.json in Chipyard
        manifest: PathBuf,
        /// The trace, a file or a named pipe
        trace: PathBuf,
    },
}

fn main() -> ExitCode {
    let result = match Cli::parse().command {
        Command::Dump { manifest, trace } => dump(&manifest, &trace),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("eventtrace: {e}");
            ExitCode::FAILURE
        }
    }
}

fn dump(manifest: &Path, trace: &Path) -> Result<()> {
    let manifest = Manifest::load(manifest)?;
    let mut out = BufWriter::new(io::stdout().lock());
    for event in Events::open(&manifest, trace)? {
        if let Err(e) = text::write_line(&manifest, &event?, &mut out) {
            return output_error(e);
        }
    }
    out.flush().or_else(output_error)
}

/// A reader such as `head` that closes the output early ends the command without an error.
fn output_error(e: io::Error) -> Result<()> {
    match e.kind() {
        ErrorKind::BrokenPipe => Ok(()),
        _ => Err(Error(format!("cannot write the output: {e}"))),
    }
}
