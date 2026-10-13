//! `eventtrace waterfall`: converts a trace into a waterfall file for the illilouette viewer, one row per object, with
//! its stages, label, how it ended, and its events, all already derived, so the viewer applies none of the trace's
//! rules. The file is illilouette's waterfall.proto, of which proto/waterfall.proto is a copy.
//!
//! The view, as `view.rs` describes it, gives each object type to draw, in the order the viewer offers them, with the
//! field whose value is an object's stage, its label, the instruction to disassemble, the event that ends an object
//! normally, and the field naming its parent. Rows are in parent order: objects created before the trace begins
//! first, by type and ID, then the others in the order they were created; each followed by its children, the objects
//! naming it as their parent, in the same order. An object whose parent is not in the file is a root. An object set
//! to a stage at cycle c is in it from cycle c+1, and one destroyed at cycle c leaves after cycle c; it is held in a
//! stage for every cycle but the last, in which it moves on.

use std::collections::HashMap;
use std::io::Write;
use std::path::PathBuf;

use eventtrace::manifest::{FieldSchema, FieldType, Lifecycle};
use eventtrace::objects::Key;
use eventtrace::text;
use eventtrace::{Error, Event, Events, Manifest, Result, Value};
use indexmap::IndexMap;
use prost::Message;

use crate::disasm::Disassembler;
use crate::output;
use crate::view::{self, View};

/// illilouette's messages, generated from proto/waterfall.proto.
mod pb {
    include!(concat!(env!("OUT_DIR"), "/illilouette.rs"));
}

use pb::event::Effect;

const FORMAT_VERSION: u32 = 1;

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
    let manifest = Manifest::load(&args.manifest)?;
    let views = view::load(&args.view, &manifest)?;
    let events = Events::open(&manifest, &args.trace)?;
    let out = output::create(&args.output)?;
    // The viewer titles the file with the trace's name.
    let title = args.trace.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default();
    convert(&views, &manifest, events, title, out)
}

/// What a view draws of its object type.
struct Drawn<'a> {
    view: &'a View,
    /// Its index in the file's types.
    index: u32,
    /// The fields of its events, by name.
    field_types: HashMap<&'a str, &'a FieldSchema>,
    stage_of: HashMap<Value, u32>,
    dasm: Option<Disassembler>,
}

/// What a row needs besides its message: whether its create event is in the trace, the order of that event among the
/// file's, its parent, and its fields so far, for its label.
#[derive(Default)]
struct Origin<'a> {
    created: Option<usize>,
    parent: Option<Key<'a>>,
    known: HashMap<&'a str, Value>,
}

fn convert<W: Write>(
    views: &[View],
    manifest: &Manifest,
    events: impl Iterator<Item = Result<Event>>,
    title: String,
    mut out: W,
) -> Result<()> {
    // The whole file is one message, so the drawn objects' events are read first.
    let (mut first, mut last) = (None, None);
    let mut mine = Vec::new();
    for event in events {
        let event = event?;
        first.get_or_insert(event.cycle);
        last = Some(event.cycle);
        if Key::subject(manifest, &event).is_some_and(|key| views.iter().any(|v| v.object == key.object_type)) {
            mine.push(event);
        }
    }

    let mut texts = Texts { index: IndexMap::from([(String::new(), 0)]) };
    let mut file = pb::Waterfall { version: FORMAT_VERSION, title, ..Default::default() };
    for site in &manifest.sites {
        let event = texts.get(&site.event_name);
        let instance = texts.get(&site.instance);
        let source = texts.get(&site.source);
        file.sites.push(pb::Site { event, instance, source });
    }
    let mut drawn: IndexMap<&str, Drawn> = IndexMap::new();
    for (index, view) in views.iter().enumerate() {
        let object = view.object.as_str();
        let track = view.tracks.as_str();
        let mut field_types: HashMap<&str, &FieldSchema> = HashMap::new();
        for event in manifest.events.iter().filter(|e| e.subject.as_deref() == Some(object)) {
            for field in &event.fields {
                field_types.insert(&field.name, field);
            }
        }
        let track_type = *field_types
            .get(track)
            .ok_or_else(|| Error(format!("no event of {object} sets a field named {track}")))?;
        // Stages: an enum's variants in their order; otherwise the values seen, in numeric order.
        let stage_values: Vec<Value> = match (track_type.ty, &track_type.enum_name) {
            (FieldType::Enum, Some(enum_name)) => manifest
                .enums
                .iter()
                .find(|e| &e.name == enum_name)
                .map(|e| e.values.values().map(|&v| Value::Unsigned(v)).collect())
                .unwrap_or_default(),
            _ => {
                let of_object = mine.iter().filter(|e| Key::subject(manifest, e).unwrap().object_type == object);
                let mut values: Vec<Value> = of_object.filter_map(|e| e.get(manifest, track)).collect();
                values.sort_by_key(|v| match *v {
                    Value::Unsigned(v) => i128::from(v),
                    Value::Signed(v) => i128::from(v),
                });
                values.dedup();
                values
            }
        };
        let stage_of = stage_values.iter().enumerate().map(|(i, &v)| (v, i as u32)).collect();
        let name = texts.get(object);
        let stages = stage_values.iter().map(|&v| texts.get(&text::value(manifest, track_type, v).to_string())).collect();
        file.types.push(pb::Type { name, stages });
        let dasm = view.disassemble.as_ref().map(|d| Disassembler::new(&d.isa)).transpose()?;
        drawn.insert(object, Drawn { view, index: index as u32, field_types, stage_of, dasm });
    }
    let text_of = |d: &Drawn, name: &str, value: Value| text::value(manifest, d.field_types[name], value).to_string();
    let present = |e: &Event| -> Vec<(&str, Value)> {
        e.present_fields(manifest).map(|(f, v)| (f.name.as_str(), v)).collect()
    };

    let mut rows: IndexMap<Key, pb::Row> = IndexMap::new();
    let mut origins: HashMap<Key, Origin> = HashMap::new();
    let mut refs = Vec::new(); // (object, event, field, object named), resolved once every row exists
    for (order, e) in mine.iter().enumerate() {
        let key = Key::subject(manifest, e).unwrap();
        let d = &drawn[key.object_type];
        let track = d.view.tracks.as_str();
        let schema = &manifest.events[e.event];
        let row = rows.entry(key).or_insert_with(|| pb::Row { r#type: d.index, id: key.id, ..Default::default() });
        let origin = origins.entry(key).or_default();
        let lifecycle = schema.lifecycle;
        let mut event = pb::Event { cycle: e.cycle, site: e.site_id, ..Default::default() };
        if lifecycle == Some(Lifecycle::Destroy) {
            event.effect = Some(Effect::Ends(true));
            row.end = Some(e.cycle + 1);
            let short = schema.short_name();
            row.ended_by = texts.get(short);
            row.normal_end = d.view.normal_end.as_deref().is_none_or(|normal| normal == short);
        } else if let Some(value) = e.get(manifest, track) {
            let stage = *d
                .stage_of
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
            let value_text = texts.get(&text_of(d, name, value));
            event.fields.push(pb::Field { name: name_text, text: value_text, row: None });
            let field = d.field_types[name];
            if let (FieldType::Id, Some(object_type), Some(id)) = (field.ty, &field.object, value.unsigned()) {
                refs.push((key, row.events.len(), event.fields.len() - 1, Key { object_type, id }));
            }
        }
        origin.known.extend(fields.iter().copied());
        if lifecycle == Some(Lifecycle::Create) {
            origin.created = Some(order);
            if let Some(parent) = &d.view.parent {
                let field = d.field_types[parent.as_str()];
                let id = e.get(manifest, parent).and_then(Value::unsigned);
                origin.parent = field.object.as_deref().zip(id).map(|(object_type, id)| Key { object_type, id });
            }
            for &(name, value) in fields.iter().filter(|(name, _)| *name != track) {
                let name_text = texts.get(name);
                let value_text = texts.get(&text_of(d, name, value));
                row.fields.push(pb::Field { name: name_text, text: value_text, row: None });
            }
        }
        row.events.push(event);
    }

    // Parent order: the roots, objects created before the trace by type and ID, then the others as created, each
    // followed by its children in the order they were created.
    let mut before: Vec<Key> = rows.keys().copied().filter(|k| origins[k].created.is_none()).collect();
    before.sort_by_key(|k| (drawn[k.object_type].index, k.id));
    let mut created: Vec<Key> = rows.keys().copied().filter(|k| origins[k].created.is_some()).collect();
    created.sort_by_key(|k| origins[k].created);
    let mut children: HashMap<Key, Vec<Key>> = HashMap::new();
    let mut roots = before;
    for key in created {
        match origins[&key].parent.filter(|p| rows.contains_key(p)) {
            Some(parent) => children.entry(parent).or_default().push(key),
            None => roots.push(key),
        }
    }
    let mut order: Vec<Key> = Vec::with_capacity(rows.len());
    let mut pending: Vec<Key> = roots.into_iter().rev().collect();
    while let Some(key) = pending.pop() {
        order.push(key);
        pending.extend(children.get(&key).into_iter().flatten().rev());
    }
    let index: HashMap<Key, u64> = order.iter().enumerate().map(|(i, &key)| (key, i as u64)).collect();
    for (key, event, field, other) in refs {
        if let Some(&row) = index.get(&other) {
            rows[&key].events[event].fields[field].row = Some(row);
        }
    }
    for (key, row) in rows.iter_mut() {
        let d = drawn.get_mut(key.object_type).unwrap();
        let origin = &origins[key];
        row.parent = origin.parent.and_then(|parent| index.get(&parent).copied());
        let fields = &origin.known;
        for name in d.view.label.fields() {
            row.label.push(fields.get(name).map_or(0, |&value| texts.get(&text_of(d, name, value))));
        }
        if let (Some(dis), Some(dasm)) = (&d.view.disassemble, d.dasm.as_mut()) {
            let bits = fields.get(dis.bits.as_str()).and_then(|v| v.unsigned());
            let pc = fields.get(dis.pc.as_str()).and_then(|v| v.unsigned());
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

    file.rows = order.iter().map(|key| rows.swap_remove(key).unwrap()).collect();
    file.first_cycle = first.unwrap_or(0);
    file.last_cycle = last.unwrap_or(0);
    file.strings = texts.index.into_keys().collect();
    out.write_all(&file.encode_to_vec()).and_then(|()| out.flush()).map_err(output::error)
}
