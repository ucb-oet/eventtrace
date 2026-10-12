//! The files the commands write, and their errors as messages for the user.

use std::fs::File;
use std::io::{self, BufWriter};
use std::path::Path;

use eventtrace::{Error, Result};

/// The file at `path`, created or emptied, to write through a buffer.
pub fn create(path: &Path) -> Result<BufWriter<File>> {
    let file = File::create(path).map_err(|e| Error(format!("cannot create {}: {e}", path.display())))?;
    Ok(BufWriter::new(file))
}

pub fn error(e: io::Error) -> Error {
    Error(format!("cannot write the output: {e}"))
}
