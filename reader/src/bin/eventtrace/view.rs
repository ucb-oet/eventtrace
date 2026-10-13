//! A design's view: a small JSON file giving how to draw its objects, beyond what the manifest says, as in
//!
//!   {"tracks": "stage", "label": "pc", "hide": ["retire"], "normal_end": "retire",
//!    "disassemble": {"bits": "bits", "pc": "pc", "isa": "rv32i"}}
//!
//! - object, optional: the object type to draw, which the manifest gives when it has only one.
//! - tracks: the field whose value is the object's stage, or in Perfetto its track. An enum field's stages follow its
//!   variants' order.
//! - label, optional: the field, or list of fields, labelling each object; by default, in Perfetto, its ID.
//! - hide, optional: in Perfetto, events of the object neither marked nor drawing arrows.
//! - normal_end, optional: in a waterfall, the event that ends an object normally, such as retire; objects ended by
//!   any other event left without it. Without it, every end is normal.
//! - disassemble, optional: the fields holding a RISC-V instruction and its PC, and the core's ISA string, of which
//!   only XLEN and the C extension change decoding. Each object's label is followed by the instruction, with a
//!   PC-relative target made absolute, as in "0x80000000  j 0x80000050".

use std::path::Path;

use eventtrace::{Error, Manifest, Result};
use serde::Deserialize;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct View {
    pub object: Option<String>,
    pub tracks: String,
    #[serde(default)]
    pub label: Labels,
    #[serde(default)]
    pub hide: Vec<String>,
    pub normal_end: Option<String>,
    pub disassemble: Option<Disassemble>,
}

/// One label field or several.
#[derive(Deserialize, Default)]
#[serde(untagged)]
pub enum Labels {
    #[default]
    None,
    One(String),
    Many(Vec<String>),
}

impl Labels {
    pub fn fields(&self) -> Vec<&str> {
        match self {
            Labels::None => vec![],
            Labels::One(field) => vec![field],
            Labels::Many(fields) => fields.iter().map(String::as_str).collect(),
        }
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Disassemble {
    pub bits: String,
    pub pc: String,
    pub isa: String,
}

impl View {
    pub fn load(path: &Path) -> Result<View> {
        let bytes = std::fs::read(path).map_err(|e| Error(format!("cannot read {}: {e}", path.display())))?;
        serde_json::from_slice(&bytes).map_err(|e| Error(format!("{} is not a view: {e}", path.display())))
    }

    /// The object type to draw: the view's, or the manifest's only one.
    pub fn object<'a>(&'a self, manifest: &'a Manifest) -> Result<&'a str> {
        if let Some(object) = &self.object {
            return Ok(object);
        }
        let mut types: Vec<&str> = manifest.events.iter().filter_map(|e| e.subject.as_deref()).collect();
        types.sort();
        types.dedup();
        match types.as_slice() {
            [only] => Ok(only),
            types => Err(Error(format!("the manifest has object types {types:?}, so the view must name one as `object`"))),
        }
    }
}
