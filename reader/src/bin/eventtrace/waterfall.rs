//! `eventtrace waterfall`: converts a trace into a waterfall file for the illilouette viewer, one row per object, with
//! its stages, label, how it ended, and its events, all already derived, so the viewer applies none of the trace's
//! rules. The file is illilouette's waterfall.proto, of which proto/waterfall.proto is a copy.
//!
//! The view, as `view.rs` describes it, gives the field whose value is an object's stage, its label, the instruction
//! to disassemble, and the event that ends an object normally. Rows are in ID order, the order in which objects were
//! created. An object set to a stage at cycle c is in it from cycle c+1, and one destroyed at cycle c leaves after
//! cycle c; it is held in a stage for every cycle but the last, in which it moves on.

use std::collections::HashMap;
use std::io::Write;
use std::path::PathBuf;

use eventtrace::manifest::{FieldSchema, FieldType, Lifecycle};
use eventtrace::objects::Key;
use eventtrace::text::{self, Style};
use eventtrace::{Error, Event, Events, Manifest, Result, Value};
use indexmap::IndexMap;
use prost::Message;

use crate::disasm::Disassembler;
use crate::output;
use crate::view::View;

/// illilouette's messages, generated from proto/waterfall.proto.
mod pb {
    include!(concat!(env!("OUT_DIR"), "/illilouette.rs"));
}

use pb::event::Effect;

const FORMAT_VERSION: u32 = 1;

/// Values as text, with addresses in hex, and object IDs without their type, since a file draws one type of object.
const STYLE: Style = Style { bare_ids: true, wide_hex: true };

/// The file's text table: each text once, index 0 the empty text.
struct Texts {
    index: IndexMap<String, u32>,
}

impl Texts {
    fn get(&mut self, text: &str) -> u32 {
        if let Some(&i) = self.index.get(text) {
            return i;
        }
        let i = self.index.len() as u32;
        self.index.insert(text.to_string(), i);
        i
    }
}

#[derive(clap::Args)]
pub struct Args {
    /// The design's view, such as reader/sodor/view.json
    view: PathBuf,
    /// The design's manifest
    manifest: PathBuf,
    /// The trace, a file or a named pipe
    trace: PathBuf,
    /// The waterfall file to write
    output: PathBuf,
}

pub fn run(args: Args) -> Result<()> {
    let view = View::load(&args.view)?;
    let manifest = Manifest::load(&args.manifest)?;
    let events = Events::open(&manifest, &args.trace)?;
    let out = output::create(&args.output)?;
    // The viewer titles the file with the trace's name.
    let title = args.trace.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default();
    convert(&view, &manifest, events, title, out)
}

fn convert<W: Write>(
    view: &View,
    manifest: &Manifest,
    events: impl Iterator<Item = Result<Event>>,
    title: String,
    mut out: W,
) -> Result<()> {
    let object = view.object(manifest)?;
    let track = view.tracks.as_str();
    // The fields of the object's events, by name.
    let mut field_types: HashMap<&str, &FieldSchema> = HashMap::new();
    for event in manifest.events.iter().filter(|e| e.subject.as_deref() == Some(object)) {
        for field in &event.fields {
            field_types.insert(&field.name, field);
        }
    }
    let track_type = *field_types
        .get(track)
        .ok_or_else(|| Error(format!("no event of {object} sets a field named {track}")))?;
    let text_of = |name: &str, value: Value| text::value(manifest, field_types[name], value, STYLE).to_string();
    let present = |e: &Event| -> Vec<(&str, Value)> {
        e.present_fields(manifest).map(|(f, v)| (f.name.as_str(), v)).collect()
    };

    // The whole file is one message, so the object's events are read first.
    let (mut first, mut last) = (None, None);
    let mut mine = Vec::new();
    for event in events {
        let event = event?;
        first.get_or_insert(event.cycle);
        last = Some(event.cycle);
        if Key::subject(manifest, &event).is_some_and(|key| key.object_type == object) {
            mine.push(event);
        }
    }

    // Stages: an enum's variants in their order; otherwise the values seen, in numeric order.
    let stage_values: Vec<Value> = match (track_type.ty, &track_type.enum_name) {
        (FieldType::Enum, Some(enum_name)) => manifest
            .enums
            .iter()
            .find(|e| &e.name == enum_name)
            .map(|e| e.values.values().map(|&v| Value::Unsigned(v)).collect())
            .unwrap_or_default(),
        _ => {
            let mut values: Vec<Value> = mine.iter().filter_map(|e| e.get(manifest, track)).collect();
            values.sort_by_key(|v| match *v {
                Value::Unsigned(v) => i128::from(v),
                Value::Signed(v) => i128::from(v),
            });
            values.dedup();
            values
        }
    };
    let stage_of: HashMap<Value, u32> = stage_values.iter().enumerate().map(|(i, &v)| (v, i as u32)).collect();

    let mut texts = Texts { index: IndexMap::from([(String::new(), 0)]) };
    let mut file = pb::Waterfall { version: FORMAT_VERSION, title, ..Default::default() };
    for site in &manifest.sites {
        let event = texts.get(&site.event_name);
        let instance = texts.get(&site.instance);
        let source = texts.get(&site.source);
        file.sites.push(pb::Site { event, instance, source });
    }
    let name = texts.get(object);
    let stages = stage_values.iter().map(|&v| texts.get(&text_of(track, v))).collect();
    file.types.push(pb::Type { name, stages });

    let mut rows: IndexMap<u64, pb::Row> = IndexMap::new();
    let mut known: HashMap<u64, HashMap<&str, Value>> = HashMap::new(); // each object's fields so far, for its label
    let mut refs = Vec::new(); // (object, event, field, object named), resolved once every row exists
    for e in &mine {
        let schema = &manifest.events[e.event];
        let id = e.subject.unwrap_or(0);
        let row = rows.entry(id).or_insert_with(|| pb::Row { r#type: 0, id, ..Default::default() });
        let lifecycle = schema.lifecycle;
        let mut event = pb::Event { cycle: e.cycle, site: e.site_id, ..Default::default() };
        if lifecycle == Some(Lifecycle::Destroy) {
            event.effect = Some(Effect::Ends(true));
            row.end = Some(e.cycle + 1);
            let short = schema.short_name();
            row.ended_by = texts.get(short);
            row.normal_end = view.normal_end.as_deref().is_none_or(|normal| normal == short);
        } else if let Some(value) = e.get(manifest, track) {
            let stage = *stage_of
                .get(&value)
                .ok_or_else(|| Error(format!("{track} {value} at cycle {} is not one of the stages", e.cycle)))?;
            event.effect = Some(Effect::Enters(stage));
            row.segments.push(pb::Segment { from: e.cycle + 1, stage, held: 0 });
        } else {
            event.effect = Some(Effect::Observes(true));
        }
        let fields = present(e);
        for &(name, value) in &fields {
            if name == track && lifecycle != Some(Lifecycle::Destroy) {
                continue;
            }
            let name_text = texts.get(name);
            let value_text = texts.get(&text_of(name, value));
            event.fields.push(pb::Field { name: name_text, text: value_text, row: None });
            if let (FieldType::Id, Some(other)) = (field_types[name].ty, value.unsigned()) {
                refs.push((id, row.events.len(), event.fields.len() - 1, other));
            }
        }
        known.entry(id).or_default().extend(fields.iter().copied());
        if lifecycle == Some(Lifecycle::Create) {
            for &(name, value) in fields.iter().filter(|(name, _)| *name != track) {
                let name_text = texts.get(name);
                let value_text = texts.get(&text_of(name, value));
                row.fields.push(pb::Field { name: name_text, text: value_text, row: None });
            }
        }
        row.events.push(event);
    }

    let mut order: Vec<u64> = rows.keys().copied().collect();
    order.sort();
    let index: HashMap<u64, u64> = order.iter().enumerate().map(|(i, &id)| (id, i as u64)).collect();
    for (id, event, field, other) in refs {
        if let Some(&row) = index.get(&other) {
            rows[&id].events[event].fields[field].row = Some(row);
        }
    }
    let labels = view.label.fields();
    let mut dasm = view.disassemble.as_ref().map(|d| Disassembler::new(&d.isa)).transpose()?;
    for (id, row) in rows.iter_mut() {
        let fields = &known[id];
        for &name in &labels {
            row.label.push(fields.get(name).map_or(0, |&value| texts.get(&text_of(name, value))));
        }
        if let (Some(d), Some(dasm)) = (&view.disassemble, dasm.as_mut()) {
            let bits = fields.get(d.bits.as_str()).and_then(|v| v.unsigned());
            let pc = fields.get(d.pc.as_str()).and_then(|v| v.unsigned());
            row.label.push(match (bits, pc) {
                (Some(bits), Some(pc)) => texts.get(dasm.text(bits, pc)),
                _ => 0,
            });
        }
        // Held: every cycle in a stage but the last. The last stage of an object still live has no known last cycle.
        for i in 0..row.segments.len() {
            let to = row.segments.get(i + 1).map(|next| next.from).or(row.end);
            let from = row.segments[i].from;
            row.segments[i].held = to.map_or(0, |to| to.saturating_sub(from).saturating_sub(1));
        }
    }

    file.rows = order.iter().map(|id| rows.swap_remove(id).unwrap()).collect();
    file.first_cycle = first.unwrap_or(0);
    file.last_cycle = last.unwrap_or(0);
    file.strings = texts.index.into_keys().collect();
    out.write_all(&file.encode_to_vec()).and_then(|()| out.flush()).map_err(output::error)
}
