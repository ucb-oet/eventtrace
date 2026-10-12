//! An event as one line of text: its cycle, IDs, name, subject, and present fields, with enums by variant name and
//! object IDs as `type#id`, as in
//!
//!   eventtrace cycle=3 event_id=0 site_id=0 instruction.decode subject=instruction#0 odd=0 pc=0 stage=Decode

use std::io::{self, Write};

use crate::manifest::{FieldType, Manifest};
use crate::trace::{Event, Value};

pub fn write_line(manifest: &Manifest, event: &Event, out: &mut impl Write) -> io::Result<()> {
    let schema = &manifest.events[event.event];
    write!(out, "eventtrace cycle={} event_id={} site_id={} {}", event.cycle, event.event_id, event.site_id, schema.name)?;
    if let (Some(object), Some(id)) = (&schema.subject, event.subject) {
        write!(out, " subject={object}#{id}")?;
    }
    for (field, value) in schema.fields.iter().zip(&event.values) {
        let Some(value) = value else { continue };
        match (field.ty, value) {
            (FieldType::Enum, Value::Unsigned(v)) => {
                let enum_name = field.enum_name.as_deref().unwrap_or("");
                match manifest.variant(enum_name, *v) {
                    Some(variant) => write!(out, " {}={variant}", field.name)?,
                    None => write!(out, " {}={enum_name}({v})", field.name)?,
                }
            }
            (FieldType::Id, Value::Unsigned(v)) => {
                write!(out, " {}={}#{v}", field.name, field.object.as_deref().unwrap_or(""))?
            }
            _ => write!(out, " {}={value}", field.name)?,
        }
    }
    writeln!(out)
}
