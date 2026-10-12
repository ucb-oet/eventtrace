//! Converts a trace into a Perfetto trace of resources: one track per value of an object field, with a slice for each
//! object while it holds that value. Open the output at https://ui.perfetto.dev.
//!
//! A view, a small JSON file per design, gives what the manifest cannot:
//!
//!   {"tracks": "stage", "label": "pc", "hide": ["retire"],
//!    "disassemble": {"bits": "bits", "pc": "pc", "isa": "rv32i"}}
//!
//! - object, optional: the object type to draw, which the manifest gives when it has only one.
//! - tracks: the field whose value is the track an object is on. An enum field's tracks follow its variants' order.
//! - label, optional: the field naming each slice, by default the object's ID. Every field an object's create event
//!   sets, but the track, becomes an argument of its slices.
//! - hide, optional: events of the object not to mark. The others that neither create the object nor carry the track
//!   field are marked at their cycle, on the subject's track; an Id field of a marked event naming another object of
//!   the type draws an arrow from a mark on that object's track, such as "stall of instruction#13" on its blocker's.
//! - disassemble, optional: the create event's fields holding a RISC-V instruction and its PC, and the core's ISA
//!   string, of which only XLEN and the C extension change decoding. Each slice's label is followed by the
//!   instruction, as in "0x80000000  j 0x80000050", with a PC-relative target made absolute.
//!
//! An object set to a value at cycle c holds it from cycle c+1, and leaves it at the cycle after the event that moves
//! or destroys it. Time is one nanosecond per cycle, so Perfetto's time axis reads in cycles. Objects that hold one
//! value at the same time get lanes: a track gains a sibling while they overlap.

use std::collections::HashMap;
use std::io::{self, Write};
use std::path::Path;

use eventtrace::manifest::{EventSchema, FieldSchema, FieldType, Lifecycle};
use eventtrace::objects::{Cycles, Key, Objects};
use eventtrace::{Error, Event, Manifest, Result, Value};
use indexmap::IndexMap;
use prost::Message;
use serde::Deserialize;

use crate::disasm::Disassembler;

/// Perfetto's messages, generated from proto/perfetto.proto.
mod pb {
    include!(concat!(env!("OUT_DIR"), "/perfetto.protos.rs"));
}

use pb::track_event::Type;

const SEQUENCE: u32 = 1;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct View {
    object: Option<String>,
    tracks: String,
    label: Option<String>,
    #[serde(default)]
    hide: Vec<String>,
    disassemble: Option<Disassemble>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Disassemble {
    bits: String,
    pc: String,
    isa: String,
}

impl View {
    pub fn load(path: &Path) -> Result<View> {
        let bytes = std::fs::read(path).map_err(|e| Error(format!("cannot read {}: {e}", path.display())))?;
        serde_json::from_slice(&bytes).map_err(|e| Error(format!("{} is not a view: {e}", path.display())))
    }
}

/// Writes Perfetto TracePackets: track descriptors, then slices and marks in time order.
struct Writer<W: Write> {
    out: W,
    next_uuid: u64,
}

impl<W: Write> Writer<W> {
    fn packet(&mut self, mut packet: pb::TracePacket) -> io::Result<()> {
        packet.trusted_packet_sequence_id = Some(SEQUENCE);
        // A Perfetto trace is one Trace message, whose packets are a repeated field, so Trace messages of one packet
        // each concatenate into the trace.
        self.out.write_all(&pb::Trace { packet: vec![packet] }.encode_to_vec())
    }

    fn track(&mut self, name: String, parent: Option<u64>, rank: Option<i32>, explicit_children: bool) -> io::Result<u64> {
        let uuid = self.next_uuid;
        self.next_uuid += 1;
        let descriptor = pb::TrackDescriptor {
            uuid: Some(uuid),
            name: Some(name),
            parent_uuid: parent,
            child_ordering: explicit_children.then_some(pb::track_descriptor::ChildTracksOrdering::Explicit as i32),
            sibling_order_rank: rank,
        };
        self.packet(pb::TracePacket { track_descriptor: Some(descriptor), ..Default::default() })?;
        Ok(uuid)
    }

    #[allow(clippy::too_many_arguments)]
    fn event(
        &mut self,
        cycle: u64,
        kind: Type,
        track: u64,
        name: Option<String>,
        args: Vec<(String, String)>,
        flows: Vec<u64>,
        terminating: Vec<u64>,
    ) -> io::Result<()> {
        let event = pb::TrackEvent {
            debug_annotations: args
                .into_iter()
                .map(|(name, value)| pb::DebugAnnotation { name: Some(name), string_value: Some(value) })
                .collect(),
            r#type: Some(kind as i32),
            track_uuid: Some(track),
            name,
            flow_ids: flows,
            terminating_flow_ids: terminating,
        };
        self.packet(pb::TracePacket { timestamp: Some(cycle), track_event: Some(event), ..Default::default() })
    }
}

/// The tracks of one field's values, each with as many lanes as objects that hold the value at once.
struct Resources {
    parent: u64,
    ranks: HashMap<Value, usize>,
    lanes: HashMap<Value, Vec<bool>>,
    uuids: HashMap<(Value, usize), u64>,
}

impl Resources {
    /// A free lane of `value`'s track, adding one if every lane is taken: the lane and its track.
    fn take<W: Write>(&mut self, writer: &mut Writer<W>, value: Value, title: &str) -> io::Result<(usize, u64)> {
        let lanes = self.lanes.entry(value).or_default();
        let lane = lanes.iter().position(|taken| !taken).unwrap_or(lanes.len());
        if lane == lanes.len() {
            lanes.push(false);
            let name = if lane == 0 { title.to_string() } else { format!("{title} {}", lane + 1) };
            let rank = self.ranks.get(&value).copied().unwrap_or(self.ranks.len()) * 1024 + lane;
            let uuid = writer.track(name, Some(self.parent), Some(rank as i32), false)?;
            self.uuids.insert((value, lane), uuid);
        }
        self.lanes.get_mut(&value).unwrap()[lane] = true;
        Ok((lane, self.uuids[&(value, lane)]))
    }

    fn release(&mut self, value: Value, lane: usize) {
        self.lanes.get_mut(&value).unwrap()[lane] = false;
    }
}

/// An object while it holds a value of the track field.
struct Slice {
    value: Value,
    lane: usize,
    track: u64,
}

/// A field's value as text: an enum by variant name, an object by `type#id`, and a UInt wider than 16 bits in hex.
fn text(manifest: &Manifest, field: &FieldSchema, value: Value) -> String {
    match (field.ty, value) {
        (FieldType::Enum, Value::Unsigned(v)) => {
            let enum_name = field.enum_name.as_deref().unwrap_or("");
            manifest.variant(enum_name, v).map(str::to_string).unwrap_or_else(|| format!("{enum_name}({v})"))
        }
        (FieldType::Id, Value::Unsigned(v)) => format!("{}#{v}", field.object.as_deref().unwrap_or("")),
        (FieldType::UInt, Value::Unsigned(v)) if field.width.unwrap_or(0) > 16 => {
            format!("0x{v:0width$x}", width = (field.width.unwrap() as usize).div_ceil(4))
        }
        _ => value.to_string(),
    }
}

/// An event's present fields, as (name, text).
fn fields(manifest: &Manifest, event: &Event) -> Vec<(String, String)> {
    let schema = &manifest.events[event.event];
    schema
        .fields
        .iter()
        .zip(&event.values)
        .filter_map(|(f, v)| v.map(|v| (f.name.clone(), text(manifest, f, v))))
        .collect()
}

/// The event's name without its object type, as in "stall" for "instruction.stall".
fn short(schema: &EventSchema) -> &str {
    schema.name.split_once('.').map_or(schema.name.as_str(), |(_, short)| short)
}

pub fn convert<W: Write>(
    view: &View,
    manifest: &Manifest,
    events: impl Iterator<Item = Result<Event>>,
    out: W,
) -> Result<()> {
    let object_types: Vec<&str> = {
        let mut types: Vec<&str> = manifest.events.iter().filter_map(|e| e.subject.as_deref()).collect();
        types.sort();
        types.dedup();
        types
    };
    let object = match (&view.object, object_types.as_slice()) {
        (Some(object), _) => object.as_str(),
        (None, [only]) => only,
        (None, types) => {
            return Err(Error(format!("the manifest has object types {types:?}, so the view must name one as `object`")));
        }
    };
    let of_object = |e: &&EventSchema| e.subject.as_deref() == Some(object);
    let track_field = manifest
        .events
        .iter()
        .filter(of_object)
        .find_map(|e| e.fields.iter().find(|f| f.name == view.tracks))
        .ok_or_else(|| Error(format!("no event of {object} has a field {}", view.tracks)))?;
    for hidden in &view.hide {
        if !manifest.events.iter().filter(of_object).any(|e| short(e) == hidden) {
            return Err(Error(format!("the view hides {hidden}, which is not an event of {object}")));
        }
    }
    // The events marked as instants, with the Id fields of each that draw arrows.
    let marks: HashMap<usize, Vec<&str>> = manifest
        .events
        .iter()
        .enumerate()
        .filter(|(_, e)| of_object(e))
        .filter(|(_, e)| e.lifecycle != Some(Lifecycle::Create) && !e.fields.iter().any(|f| f.name == view.tracks))
        .filter(|(_, e)| !view.hide.iter().any(|h| h == short(e)))
        .map(|(i, e)| {
            let ids = e.fields.iter().filter(|f| f.ty == FieldType::Id && f.object.as_deref() == Some(object));
            (i, ids.map(|f| f.name.as_str()).collect())
        })
        .collect();
    let ranks = match (track_field.ty, &track_field.enum_name) {
        (FieldType::Enum, Some(enum_name)) => manifest
            .enums
            .iter()
            .find(|e| &e.name == enum_name)
            .map(|e| e.values.values().enumerate().map(|(i, &v)| (Value::Unsigned(v), i)).collect())
            .unwrap_or_default(),
        _ => HashMap::new(),
    };
    let mut dasm = view.disassemble.as_ref().map(|d| Disassembler::new(&d.isa)).transpose()?;

    let io = |e: io::Error| Error(format!("cannot write the output: {e}"));
    let mut writer = Writer { out, next_uuid: 1 };
    let parent = writer.track(format!("{object} {}", view.tracks), None, None, true).map_err(io)?;
    let mut resources = Resources { parent, ranks, lanes: HashMap::new(), uuids: HashMap::new() };
    let mut objects = Objects::new(manifest);
    let mut slices: IndexMap<u64, Slice> = IndexMap::new();
    let mut next_flow = 1;
    let mut last = 0;

    for cycle in Cycles::new(events) {
        let cycle = cycle?;
        last = cycle.cycle;
        // Every event of a cycle sees the state before its clock edge; their changes start at the next cycle.
        let mut ends: Vec<(u64, Option<&str>)> = Vec::new();
        let mut begins: Vec<(u64, Value)> = Vec::new();
        for e in &cycle.events {
            let Some(key) = Key::subject(manifest, e) else { continue };
            if key.object_type != object {
                continue;
            }
            let schema = &manifest.events[e.event];
            if let (Some(ids), Some(slice)) = (marks.get(&e.event), slices.get(&key.id)) {
                let track = slice.track;
                let mut terminating = Vec::new();
                for id in ids {
                    let Some(other) = e.get(manifest, id).and_then(Value::unsigned) else { continue };
                    if let Some(other) = slices.get(&other) {
                        let name = format!("{} of {object}#{}", short(schema), key.id);
                        writer.event(cycle.cycle, Type::Instant, other.track, Some(name), vec![], vec![next_flow], vec![]).map_err(io)?;
                        terminating.push(next_flow);
                        next_flow += 1;
                    }
                }
                let args = fields(manifest, e);
                writer.event(cycle.cycle, Type::Instant, track, Some(short(schema).to_string()), args, vec![], terminating).map_err(io)?;
            }
            if schema.lifecycle == Some(Lifecycle::Destroy) {
                ends.push((key.id, Some(short(schema))));
            } else if let Some(value) = e.get(manifest, &view.tracks) {
                if slices.contains_key(&key.id) {
                    ends.push((key.id, None));
                }
                begins.push((key.id, value));
            }
        }
        objects.commit(&cycle);
        for (id, how) in ends {
            if let Some(slice) = slices.shift_remove(&id) {
                let args = how.map(|how| vec![("end".to_string(), how.to_string())]).unwrap_or_default();
                writer.event(cycle.cycle + 1, Type::SliceEnd, slice.track, None, args, vec![], vec![]).map_err(io)?;
                resources.release(slice.value, slice.lane);
            }
        }
        for (id, value) in begins {
            let key = Key { object_type: object, id };
            let (lane, track) = resources.take(&mut writer, value, &text(manifest, track_field, value)).map_err(io)?;
            slices.insert(id, Slice { value, lane, track });
            // The fields its create event set, but the track, and the instruction they hold.
            let mut created: Vec<(String, String)> = Vec::new();
            if let Some(create) = objects.create_event(&key) {
                created = fields(manifest, create).into_iter().filter(|(name, _)| *name != view.tracks).collect();
                if let (Some(d), Some(dasm)) = (&view.disassemble, dasm.as_mut()) {
                    let bits = create.get(manifest, &d.bits).and_then(Value::unsigned);
                    let pc = create.get(manifest, &d.pc).and_then(Value::unsigned);
                    if let (Some(bits), Some(pc)) = (bits, pc) {
                        created.push(("asm".to_string(), dasm.text(bits, pc).to_string()));
                    }
                }
            }
            let known = |name: &str| created.iter().find(|(n, _)| n == name).map(|(_, v)| v.clone());
            let mut label = view.label.as_deref().and_then(known).unwrap_or_else(|| format!("{object}#{id}"));
            if let Some(asm) = known("asm") {
                label = format!("{label}  {asm}");
            }
            let mut args = vec![("object".to_string(), format!("{object}#{id}"))];
            args.extend(created);
            writer.event(cycle.cycle + 1, Type::SliceBegin, track, Some(label), args, vec![], vec![]).map_err(io)?;
        }
    }
    // Objects still live when the trace ends keep their last slice open to its end.
    for slice in slices.values() {
        let args = vec![("end".to_string(), "live at the end of the trace".to_string())];
        writer.event(last + 1, Type::SliceEnd, slice.track, None, args, vec![], vec![]).map_err(io)?;
    }
    writer.out.flush().map_err(io)
}

