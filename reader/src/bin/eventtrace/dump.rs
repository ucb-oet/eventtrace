//! `eventtrace dump`: prints a trace as text to standard output, one line per event, as `text::write_line` writes it.

use std::io::{self, BufWriter, ErrorKind, Write};
use std::path::PathBuf;

use eventtrace::{Events, Manifest, Result, text};

use crate::output;

#[derive(clap::Args)]
pub struct Args {
    /// The design's manifest, <long name>.eventtrace.json in Chipyard
    manifest: PathBuf,
    /// The trace, a file or a named pipe
    trace: PathBuf,
}

pub fn run(args: Args) -> Result<()> {
    let manifest = Manifest::load(&args.manifest)?;
    let mut out = BufWriter::new(io::stdout().lock());
    for event in Events::open(&manifest, &args.trace)? {
        if let Err(e) = text::write_line(&manifest, &event?, &mut out) {
            return write_error(e);
        }
    }
    out.flush().or_else(write_error)
}

/// A reader such as `head` that closes the output early ends the dump without an error.
fn write_error(e: io::Error) -> Result<()> {
    match e.kind() {
        ErrorKind::BrokenPipe => Ok(()),
        _ => Err(output::error(e)),
    }
}
