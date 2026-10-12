//! Reads EventTrace traces. A trace is the stream that `eventtrace.proto` describes, and the design's manifest names
//! what its numbers refer to; `Manifest::load` reads the one, and `Events` the other with it.

pub mod manifest;
pub mod text;
pub mod trace;

pub use manifest::Manifest;
pub use trace::{Event, Events, Value};

/// The trace format's messages, generated from eventtrace.proto.
pub mod proto {
    include!(concat!(env!("OUT_DIR"), "/eventtrace.rs"));
}

/// What went wrong reading a manifest or a trace, as a message for the user.
#[derive(Debug)]
pub struct Error(pub String);

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;
