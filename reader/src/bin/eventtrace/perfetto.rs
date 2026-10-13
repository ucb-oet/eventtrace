//! `eventtrace perfetto`: converts a trace into a Perfetto trace of resources, one track per value of an object field,
//! with a slice for each object while it holds that value. Open the output at https://ui.perfetto.dev.
//!
//! The view, as `view.rs` describes it, gives the field whose values are the tracks and how to label slices. Every
//! field an object's create event sets, but the track, becomes an argument of its slices. The object's events that
//! neither create it nor carry the track field, less those the view hides, are marked at their cycle on the subject's
//! track. An Id field naming another object of the type, in any event the view does not hide, draws an arrow from a
//! mark on that object's track, such as "stall of instruction#13" on its blocker's, to the event's mark, or, for an
//! event not marked, to the slice it begins.
//!
//! An object set to a value at cycle c holds it from cycle c+1, and leaves it at the cycle after the event that moves
//! or destroys it. Time is one nanosecond per cycle, so Perfetto's time axis reads in cycles. Objects that hold one
//! value at the same time get lanes: a track gains a sibling while they overlap.

use std::collections::{HashMap, HashSet};
use std::io::Write;
use std::path::PathBuf;

use eventtrace::manifest::{EventSchema, FieldType, Lifecycle};
use eventtrace::objects::{Cycles, Key, Objects};
use eventtrace::text;
use eventtrace::{Error, Event, Events, Manifest, Result, Value};
use indexmap::IndexMap;
use prost::Message;

use crate::disasm::Disassembler;
use crate::output;
use crate::view::View;

/// Perfetto's messages, generated from proto/perfetto.proto.
mod pb {
    include!(concat!(env!("OUT_DIR"), "/perfetto.protos.rs"));
}

use pb::track_event::Type;

const SEQUENCE: u32 = 1;

/// Writes Perfetto TracePackets: track descriptors, then slices and marks in time order.
struct Writer<W: Write> {
    out: W,
    next_uuid: u64,
}

impl<W: Write> Writer<W> {
    fn packet(&mut self, mut packet: pb::TracePacket) -> Result<()> {
        packet.trusted_packet_sequence_id = Some(SEQUENCE);
        // A Perfetto trace is one Trace message, whose packets are a repeated field, so Trace messages of one packet
        // each concatenate into the trace.
        self.out.write_all(&pb::Trace { packet: vec![packet] }.encode_to_vec()).map_err(output::error)
    }

    fn track(&mut self, name: String, parent: Option<u64>, rank: Option<i32>, explicit_children: bool) -> Result<u64> {
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
    ) -> Result<()> {
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
    fn take<W: Write>(&mut self, writer: &mut Writer<W>, value: Value, title: &str) -> Result<(usize, u64)> {
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

/// An event's present fields, as (name, text).
fn fields(manifest: &Manifest, event: &Event) -> Vec<(String, String)> {
    event.present_fields(manifest).map(|(f, v)| (f.name.clone(), text::value(manifest, f, v).to_string())).collect()
}

#[derive(clap::Args)]
pub struct Args {
    /// The design's view, such as reader/sodor/view.json
    view: PathBuf,
    /// The design's manifest
    manifest: PathBuf,
    /// The trace, a file or a named pipe
    trace: PathBuf,
    /// The Perfetto trace to write
    output: PathBuf,
}

pub fn run(args: Args) -> Result<()> {
    let view = View::load(&args.view)?;
    let manifest = Manifest::load(&args.manifest)?;
    let events = Events::open(&manifest, &args.trace)?;
    convert(&view, &manifest, events, output::create(&args.output)?)
}

fn convert<W: Write>(
    view: &View,
    manifest: &Manifest,
    events: impl Iterator<Item = Result<Event>>,
    out: W,
) -> Result<()> {
    let object = view.object(manifest)?;
    let labels = view.label.fields();
    let of_object = |e: &&EventSchema| e.subject.as_deref() == Some(object);
    let track_field = manifest
        .events
        .iter()
        .filter(of_object)
        .find_map(|e| e.fields.iter().find(|f| f.name == view.tracks))
        .ok_or_else(|| Error(format!("no event of {object} has a field {}", view.tracks)))?;
    for hidden in &view.hide {
        if !manifest.events.iter().filter(of_object).any(|e| e.short_name() == hidden) {
            return Err(Error(format!("the view hides {hidden}, which is not an event of {object}")));
        }
    }
    // The object's events that the view does not hide.
    let shown: Vec<(usize, &EventSchema)> = manifest
        .events
        .iter()
        .enumerate()
        .filter(|(_, e)| of_object(e))
        .filter(|(_, e)| !view.hide.iter().any(|h| h == e.short_name()))
        .collect();
    // The events marked as instants.
    let marked: HashSet<usize> = shown
        .iter()
        .filter(|(_, e)| e.lifecycle != Some(Lifecycle::Create) && !e.fields.iter().any(|f| f.name == view.tracks))
        .map(|&(i, _)| i)
        .collect();
    // The Id fields of each event that draw arrows.
    let arrows: HashMap<usize, Vec<&str>> = shown
        .iter()
        .map(|&(i, e)| {
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

    let mut writer = Writer { out, next_uuid: 1 };
    let parent = writer.track(format!("{object} {}", view.tracks), None, None, true)?;
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
        let mut begins: Vec<(u64, Value, Vec<u64>)> = Vec::new();
        for e in &cycle.events {
            let Some(key) = Key::subject(manifest, e) else { continue };
            if key.object_type != object {
                continue;
            }
            let schema = &manifest.events[e.event];
            let destroys = schema.lifecycle == Some(Lifecycle::Destroy);
            let value = if destroys { None } else { e.get(manifest, &view.tracks) };
            // The event's arrows end at its mark, on the subject's track, or at the slice it begins.
            let mark = slices.get(&key.id).filter(|_| marked.contains(&e.event)).map(|slice| slice.track);
            let mut flows = Vec::new();
            if mark.is_some() || value.is_some() {
                for id in arrows.get(&e.event).into_iter().flatten() {
                    let Some(other) = e.get(manifest, id).and_then(Value::unsigned) else { continue };
                    if let Some(other) = slices.get(&other) {
                        let name = format!("{} of {object}#{}", schema.short_name(), key.id);
                        writer.event(cycle.cycle, Type::Instant, other.track, Some(name), vec![], vec![next_flow], vec![])?;
                        flows.push(next_flow);
                        next_flow += 1;
                    }
                }
            }
            if let Some(track) = mark {
                let args = fields(manifest, e);
                let flows = std::mem::take(&mut flows);
                writer.event(cycle.cycle, Type::Instant, track, Some(schema.short_name().to_string()), args, vec![], flows)?;
            }
            if destroys {
                ends.push((key.id, Some(schema.short_name())));
            } else if let Some(value) = value {
                if slices.contains_key(&key.id) {
                    ends.push((key.id, None));
                }
                begins.push((key.id, value, flows));
            }
        }
        objects.commit(&cycle);
        for (id, how) in ends {
            if let Some(slice) = slices.shift_remove(&id) {
                let args = how.map(|how| vec![("end".to_string(), how.to_string())]).unwrap_or_default();
                writer.event(cycle.cycle + 1, Type::SliceEnd, slice.track, None, args, vec![], vec![])?;
                resources.release(slice.value, slice.lane);
            }
        }
        for (id, value, flows) in begins {
            let key = Key { object_type: object, id };
            let title = text::value(manifest, track_field, value).to_string();
            let (lane, track) = resources.take(&mut writer, value, &title)?;
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
            let named: Vec<String> = labels.iter().filter_map(|&name| known(name)).collect();
            let mut label = if named.is_empty() { format!("{object}#{id}") } else { named.join(" ") };
            if let Some(asm) = known("asm") {
                label = format!("{label}  {asm}");
            }
            let mut args = vec![("object".to_string(), format!("{object}#{id}"))];
            args.extend(created);
            writer.event(cycle.cycle + 1, Type::SliceBegin, track, Some(label), args, vec![], flows)?;
        }
    }
    // Objects still live when the trace ends keep their last slice open to its end.
    for slice in slices.values() {
        let args = vec![("end".to_string(), "live at the end of the trace".to_string())];
        writer.event(last + 1, Type::SliceEnd, slice.track, None, args, vec![], vec![])?;
    }
    writer.out.flush().map_err(output::error)
}

