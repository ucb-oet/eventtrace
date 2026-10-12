//! Events and field values as text. A value reads as the manifest names it, an enum by its variant's name and an
//! object ID with its type, and an output may change how IDs and wide numbers read through a `Style`. An event is one
//! line of the dump: its cycle, IDs, name, subject, and present fields, as in
//!
//!   eventtrace cycle=3 event_id=0 site_id=0 instruction.decode subject=instruction#0 odd=0 pc=0 stage=Decode

use std::fmt;
use std::io::{self, Write};

use crate::manifest::{FieldSchema, FieldType, Manifest};
use crate::trace::{Event, Value};

/// How an output writes values, where it differs from the dump.
#[derive(Clone, Copy, Default)]
pub struct Style {
    /// An object ID as `#13` rather than `instruction#13`, for an output in which its type goes without saying.
    pub bare_ids: bool,
    /// A UInt wider than 16 bits, such as an address, in hex padded to its width.
    pub wide_hex: bool,
}

/// A field's value as text: an enum by variant name, or as `enum(value)` for a value it does not name, an object ID
/// as `type#id`, and a number in decimal, unless the style says otherwise.
pub fn value<'m>(manifest: &'m Manifest, field: &'m FieldSchema, value: Value, style: Style) -> impl fmt::Display + 'm {
    Text { manifest, field, value, style }
}

struct Text<'m> {
    manifest: &'m Manifest,
    field: &'m FieldSchema,
    value: Value,
    style: Style,
}

impl fmt::Display for Text<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let field = self.field;
        match (field.ty, self.value) {
            (FieldType::Enum, Value::Unsigned(v)) => {
                let enum_name = field.enum_name.as_deref().unwrap_or("");
                match self.manifest.variant(enum_name, v) {
                    Some(variant) => f.write_str(variant),
                    None => write!(f, "{enum_name}({v})"),
                }
            }
            (FieldType::Id, Value::Unsigned(v)) if self.style.bare_ids => write!(f, "#{v}"),
            (FieldType::Id, Value::Unsigned(v)) => write!(f, "{}#{v}", field.object.as_deref().unwrap_or("")),
            (FieldType::UInt, Value::Unsigned(v)) if self.style.wide_hex && field.width.unwrap_or(0) > 16 => {
                write!(f, "0x{v:0width$x}", width = (field.width.unwrap() as usize).div_ceil(4))
            }
            (_, value) => write!(f, "{value}"),
        }
    }
}

/// Writes an event as one line of the dump.
pub fn write_line(manifest: &Manifest, event: &Event, out: &mut impl Write) -> io::Result<()> {
    let schema = &manifest.events[event.event];
    write!(out, "eventtrace cycle={} event_id={} site_id={} {}", event.cycle, event.event_id, event.site_id, schema.name)?;
    if let (Some(object), Some(id)) = (&schema.subject, event.subject) {
        write!(out, " subject={object}#{id}")?;
    }
    for (field, v) in event.present_fields(manifest) {
        write!(out, " {}={}", field.name, value(manifest, field, v, Style::default()))?;
    }
    writeln!(out)
}
