//! The `eventtrace` command: tools that read EventTrace traces, each given the design's manifest. Each subcommand is a
//! module with its arguments, `Args`, and its entry point, `run`.

mod disasm;
mod dump;
mod output;
mod perfetto;
mod view;
mod waterfall;

use std::process::ExitCode;

use clap::{Parser, Subcommand};

#[derive(Parser)]
#[command(name = "eventtrace", about = "Reads EventTrace traces")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Prints a trace as text, one line per event, with absent fields left out.
    Dump(dump::Args),
    /// Converts a trace into a Perfetto trace of resources, for https://ui.perfetto.dev: one track per value of an
    /// object field, with a slice for each object while it holds that value.
    Perfetto(perfetto::Args),
    /// Converts a trace into a waterfall file for the illilouette viewer: one row per object, with its stages,
    /// label, how it ended, and its events, all already derived.
    Waterfall(waterfall::Args),
}

fn main() -> ExitCode {
    let result = match Cli::parse().command {
        Command::Dump(args) => dump::run(args),
        Command::Perfetto(args) => perfetto::run(args),
        Command::Waterfall(args) => waterfall::run(args),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("eventtrace: {e}");
            ExitCode::FAILURE
        }
    }
}
