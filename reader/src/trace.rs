//! Reading a trace: its header's checks, then each event with its fields decoded, as eventtrace.proto describes. The
//! stream is read as it arrives, so a trace may come through a pipe.

use std::fs::File;
use std::io::{BufReader, ErrorKind, Read};
use std::path::Path;

use prost::Message;

use crate::manifest::{FieldType, Manifest};
use crate::proto::{self, record};
use crate::{Error, Result};

/// The version of eventtrace.proto that this reader knows.
pub const FORMAT_VERSION: u32 = 1;

/// One event of a trace, with its fields decoded.
#[derive(Debug, Clone)]
pub struct Event {
    pub cycle: u64,
    /// Events count from 0 through the trace.
    pub event_id: u64,
    pub site_id: u32,
    /// The event's schema, as an index into the manifest's events.
    pub event: usize,
    /// The ID of the event's subject, whose type the schema gives, if the schema gives it one.
    pub subject: Option<u64>,
    /// Each field's value in the schema's order, or None where the site left it DontCare.
    pub values: Vec<Option<Value>>,
}

/// A field's value: a Bool, UInt, Enum, or Id is unsigned, and an SInt signed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Value {
    Unsigned(u64),
    Signed(i64),
}

impl Value {
    /// The value of an unsigned field, or None for a signed one.
    pub fn unsigned(self) -> Option<u64> {
        match self {
            Value::Unsigned(v) => Some(v),
            Value::Signed(_) => None,
        }
    }
}

impl std::fmt::Display for Value {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Value::Unsigned(v) => write!(f, "{v}"),
            Value::Signed(v) => write!(f, "{v}"),
        }
    }
}

impl Event {
    /// The value of a field, by name, if the event has the field and it is present.
    pub fn get(&self, manifest: &Manifest, field: &str) -> Option<Value> {
        self.values[manifest.field_index(self.event, field)?]
    }
}

/// The events of a trace, in order.
pub struct Events<'m, R> {
    manifest: &'m Manifest,
    name: String,
    input: BufReader<R>,
    buf: Vec<u8>,
    cycle: u64,
    pending: std::vec::IntoIter<proto::Event>,
    next_id: u64,
}

impl<'m> Events<'m, File> {
    /// The events of the trace at `path`, which may be a file or a named pipe.
    pub fn open(manifest: &'m Manifest, path: &Path) -> Result<Self> {
        let file = File::open(path).map_err(|e| Error(format!("cannot open {}: {e}", path.display())))?;
        Events::new(manifest, file, path.display().to_string())
    }
}

impl<'m, R: Read> Events<'m, R> {
    /// The events of the trace that `input` delivers; `name` names it in messages. Reads the header first, checking
    /// that the trace has this reader's format version and was written by the manifest's design.
    pub fn new(manifest: &'m Manifest, input: R, name: String) -> Result<Self> {
        let mut events = Events {
            manifest,
            name,
            input: BufReader::new(input),
            buf: Vec::new(),
            cycle: 0,
            pending: Vec::new().into_iter(),
            next_id: 0,
        };
        let header = match events.record()? {
            Some(proto::Record { record: Some(record::Record::Header(header)) }) => header,
            _ => return Err(Error(format!("{} does not start with a header", events.name))),
        };
        if header.version != FORMAT_VERSION {
            return Err(Error(format!(
                "{} has format version {}; this reader knows {FORMAT_VERSION}",
                events.name, header.version
            )));
        }
        if header.manifest_sha256 != manifest.sha256 {
            return Err(Error(format!("{} was written by a design with another manifest", events.name)));
        }
        Ok(events)
    }

    /// The next Record, or None at the end of the stream.
    fn record(&mut self) -> Result<Option<proto::Record>> {
        let truncated = |e: std::io::Error, what: &str| {
            if e.kind() == ErrorKind::UnexpectedEof {
                Error(format!("{} ends inside {what}", self.name))
            } else {
                Error(format!("cannot read {}: {e}", self.name))
            }
        };
        // The length prefix, a varint.
        let (mut size, mut shift) = (0u64, 0);
        loop {
            let mut byte = [0u8];
            if let Err(e) = self.input.read_exact(&mut byte) {
                if shift == 0 && e.kind() == ErrorKind::UnexpectedEof {
                    return Ok(None);
                }
                return Err(truncated(e, "a record's length"));
            }
            size |= u64::from(byte[0] & 0x7f) << shift;
            if byte[0] & 0x80 == 0 {
                break;
            }
            shift += 7;
            if shift >= 64 {
                return Err(Error(format!("{} has a record length longer than 64 bits", self.name)));
            }
        }
        self.buf.resize(size as usize, 0);
        if let Err(e) = self.input.read_exact(&mut self.buf) {
            return Err(truncated(e, "a record"));
        }
        proto::Record::decode(self.buf.as_slice())
            .map(Some)
            .map_err(|e| Error(format!("{} has a malformed record: {e}", self.name)))
    }

    fn decode(&mut self, raw: proto::Event) -> Result<Event> {
        let event = self.manifest.site_event(raw.site_id).ok_or_else(|| {
            Error(format!("{}: cycle {} has an event from site {}, which the manifest does not list", self.name, self.cycle, raw.site_id))
        })?;
        let schema = &self.manifest.events[event];
        let mut values = raw.values.into_iter();
        let mut decoded = Vec::with_capacity(schema.fields.len());
        for (i, field) in schema.fields.iter().enumerate() {
            if i < 64 && raw.absent >> i & 1 == 1 {
                decoded.push(None);
                continue;
            }
            let Some(v) = values.next() else {
                return Err(Error(format!("{}: an event of {} at cycle {} has too few values", self.name, schema.name, self.cycle)));
            };
            // eventtrace.proto ZigZag-encodes an SInt in its uint64 values, as sint64 would.
            decoded.push(Some(if field.ty == FieldType::SInt {
                Value::Signed((v >> 1) as i64 ^ -((v & 1) as i64))
            } else {
                Value::Unsigned(v)
            }));
        }
        if values.next().is_some() {
            return Err(Error(format!("{}: an event of {} at cycle {} has too many values", self.name, schema.name, self.cycle)));
        }
        let event_id = self.next_id;
        self.next_id += 1;
        Ok(Event {
            cycle: self.cycle,
            event_id,
            site_id: raw.site_id,
            event,
            subject: schema.subject.as_ref().map(|_| raw.subject),
            values: decoded,
        })
    }
}

impl<R: Read> Iterator for Events<'_, R> {
    type Item = Result<Event>;

    fn next(&mut self) -> Option<Result<Event>> {
        loop {
            if let Some(raw) = self.pending.next() {
                return Some(self.decode(raw));
            }
            match self.record() {
                Ok(Some(proto::Record { record: Some(record::Record::Cycle(cycle)) })) => {
                    self.cycle = cycle.cycle;
                    self.pending = cycle.events.into_iter();
                }
                Ok(Some(_)) => continue,
                Ok(None) => return None,
                Err(e) => return Some(Err(e)),
            }
        }
    }
}
