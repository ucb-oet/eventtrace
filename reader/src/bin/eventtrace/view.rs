//! A design's view: a small JSON file giving how to draw its objects, beyond what the manifest says. It is one view,
//! as in
//!
//!   {"tracks": "stage", "label": "pc", "hide": ["retire"], "normal_end": "retire",
//!    "disassemble": {"bits": "bits", "pc": "pc", "isa": "rv32i"}}
//!
//! or a list of views, one per object type to draw, in the order the viewer offers the types, as in
//!
//!   [{"object": "instruction", "tracks": "stage", "label": "pc", "parent": "fetch"},
//!    {"object": "fetch", "tracks": "stage", "label": "pc"}]
//!
//! - object: the object type to draw. A single view may leave it out when the manifest has only one type.
//! - tracks: the field whose value is the object's stage, or in Perfetto its track. An enum field's stages follow its
//!   variants' order.
//! - label, optional: the field, or list of fields, labelling each object; by default, in Perfetto, its ID.
//! - hide, optional: in Perfetto, events of the object neither marked nor drawing arrows.
//! - normal_end, optional: in a waterfall, the event that ends an object normally, such as retire; objects ended by
//!   any other event left without it. Without it, every end is normal.
//! - disassemble, optional: the fields holding a RISC-V instruction and its PC, and the core's ISA string, of which
//!   only XLEN and the C extension change decoding. Each object's label is followed by the instruction, with a
//!   PC-relative target made absolute, as in "0x80000000  j 0x80000050".
//! - parent, optional: an Id field that the object's create event sets, naming the object it came from, of a type the
//!   file also draws. In a waterfall, an object's row follows its parent's.

use std::path::Path;

use eventtrace::manifest::{FieldType, Lifecycle};
use eventtrace::{Error, Manifest, Result};
use serde::Deserialize;

/// How to draw one object type.
pub struct View {
    pub object: String,
    pub tracks: String,
    pub label: Labels,
    pub hide: Vec<String>,
    pub normal_end: Option<String>,
    pub disassemble: Option<Disassemble>,
    pub parent: Option<String>,
}

/// One view as the file writes it, with its object type optional.
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Written {
    object: Option<String>,
    tracks: String,
    #[serde(default)]
    label: Labels,
    #[serde(default)]
    hide: Vec<String>,
    normal_end: Option<String>,
    disassemble: Option<Disassemble>,
    parent: Option<String>,
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

/// Loads a view file's views, each with its object type, in the file's order.
pub fn load(path: &Path, manifest: &Manifest) -> Result<Vec<View>> {
    let bytes = std::fs::read(path).map_err(|e| Error(format!("cannot read {}: {e}", path.display())))?;
    let not_a_view = |e: serde_json::Error| Error(format!("{} is not a view: {e}", path.display()));
    let json: serde_json::Value = serde_json::from_slice(&bytes).map_err(not_a_view)?;
    let views = if json.is_array() {
        let written: Vec<Written> = serde_json::from_value(json).map_err(not_a_view)?;
        if written.is_empty() {
            return Err(Error(format!("{} lists no views", path.display())));
        }
        let mut views = Vec::new();
        for w in written {
            let object = w.object.clone().ok_or_else(|| {
                Error(format!("{}: each view in a list names its `object`", path.display()))
            })?;
            if views.iter().any(|v: &View| v.object == object) {
                return Err(Error(format!("{}: two views draw {object}", path.display())));
            }
            views.push(w.into_view(object));
        }
        views
    } else {
        let written: Written = serde_json::from_value(json).map_err(not_a_view)?;
        let object = match &written.object {
            Some(object) => object.clone(),
            None => only_type(manifest)?.to_string(),
        };
        vec![written.into_view(object)]
    };
    for view in &views {
        check(view, &views, manifest)?;
    }
    Ok(views)
}

impl Written {
    fn into_view(self, object: String) -> View {
        View {
            object,
            tracks: self.tracks,
            label: self.label,
            hide: self.hide,
            normal_end: self.normal_end,
            disassemble: self.disassemble,
            parent: self.parent,
        }
    }
}

/// The manifest's object type, if it has only one.
fn only_type(manifest: &Manifest) -> Result<&str> {
    let mut types: Vec<&str> = manifest.events.iter().filter_map(|e| e.subject.as_deref()).collect();
    types.sort();
    types.dedup();
    match types.as_slice() {
        [only] => Ok(only),
        types => Err(Error(format!("the manifest has object types {types:?}, so the view must name one as `object`"))),
    }
}

/// A view's object type must have events, and its parent must be an Id field its create event sets, naming a type the
/// file also draws.
fn check(view: &View, views: &[View], manifest: &Manifest) -> Result<()> {
    let object = view.object.as_str();
    if !manifest.events.iter().any(|e| e.subject.as_deref() == Some(object)) {
        return Err(Error(format!("the view draws {object}, which no event of the manifest has as its subject")));
    }
    let Some(parent) = &view.parent else { return Ok(()) };
    let field = manifest
        .events
        .iter()
        .filter(|e| e.subject.as_deref() == Some(object) && e.lifecycle == Some(Lifecycle::Create))
        .find_map(|e| e.fields.iter().find(|f| &f.name == parent));
    match field {
        Some(f) if f.ty == FieldType::Id && views.iter().any(|v| Some(&v.object) == f.object.as_ref()) => Ok(()),
        _ => Err(Error(format!(
            "the parent of {object}, {parent}, must be an Id field that its create event sets, naming a type the view \
             draws"
        ))),
    }
}
