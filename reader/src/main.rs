//! The `eventtrace` command: tools that read EventTrace traces, each given the design's manifest.

mod disasm;
mod perfetto;

use std::fs::File;
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
    /// Converts a trace into a Perfetto trace of resources, for https://ui.perfetto.dev: one track per value of an
    /// object field, with a slice for each object while it holds that value.
    Perfetto {
        /// The design's view, such as reader/sodor/view.json
        view: PathBuf,
        /// The design's manifest
        manifest: PathBuf,
        /// The trace, a file or a named pipe
        trace: PathBuf,
        /// The Perfetto trace to write
        output: PathBuf,
    },
}

fn main() -> ExitCode {
    let result = match Cli::parse().command {
        Command::Dump { manifest, trace } => dump(&manifest, &trace),
        Command::Perfetto { view, manifest, trace, output } => convert(&view, &manifest, &trace, &output),
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

fn convert(view: &Path, manifest: &Path, trace: &Path, output: &Path) -> Result<()> {
    let view = perfetto::View::load(view)?;
    let manifest = Manifest::load(manifest)?;
    let events = Events::open(&manifest, trace)?;
    let out = File::create(output).map_err(|e| Error(format!("cannot create {}: {e}", output.display())))?;
    perfetto::convert(&view, &manifest, events, BufWriter::new(out))
}
