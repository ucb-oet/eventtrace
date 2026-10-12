#!/usr/bin/env python3
"""Converts an EventTrace trace into a Perfetto trace of resources: one track per value of an object field, with a
slice for each object while it holds that value.

  uv run --project tools/eventtrace/scripts tools/eventtrace/scripts/eventtrace_perfetto.py <view> <manifest> <trace> <output>

Open the output at https://ui.perfetto.dev. The view is a small JSON file saying how to draw one design's objects:

  {"object": "instruction", "tracks": "stage", "label": "pc",
   "instants": ["stall", "mem_wait", "squash", "flush", "trap"],
   "flows": {"stall": "blocker", "squash": "by"},
   "disassemble": {"bits": "bits", "pc": "pc", "isa": "rv32i"}}

- object: the object type to draw.
- tracks: the field whose value is the track an object is on. An enum field's tracks follow its variants' order.
- label: the field naming each slice. Every field an object's create event sets becomes an argument of its slices.
- instants: the object's events to mark at their cycle, on the subject's track.
- flows: for some of those events, an Id field naming another object. An arrow runs from an instant on that
  object's track, such as "stall of instruction#13" on its blocker's, to the subject's instant.
- disassemble, optional: the create event's fields holding a RISC-V instruction and its PC, and the core's ISA
  string, of which only XLEN and the C extension change decoding. Each slice's label is followed by the instruction,
  as in "0x80000000  j 0x80000050", with a PC-relative target made absolute.

An event takes effect after its clock edge, so an object set to a value at cycle c holds it from cycle c+1, and leaves
it at the cycle after the event that moves or destroys it. Time is one nanosecond per cycle, so Perfetto's time axis
reads in cycles. Objects that hold one value at the same time get lanes: a track gains a sibling while they overlap.

It builds Perfetto's TracePackets with the protobuf classes of the perfetto package, which uv installs, as this
directory's pyproject.toml lists it.
"""
import json
import os
import sys

import capstone
from perfetto.protos.perfetto.trace import perfetto_trace_pb2 as pb

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eventtrace_trace import Trace  # noqa: E402

SLICE_BEGIN, SLICE_END, INSTANT = pb.TrackEvent.TYPE_SLICE_BEGIN, pb.TrackEvent.TYPE_SLICE_END, pb.TrackEvent.TYPE_INSTANT
SEQUENCE = 1


class Writer:
    """Writes Perfetto TracePackets: track descriptors, then slices and instants in time order."""

    def __init__(self, out):
        self.out = out
        self.next_uuid = 1

    def _packet(self, packet):
        packet.trusted_packet_sequence_id = SEQUENCE
        # A Perfetto trace is one Trace message, whose packets are a repeated field, so Trace messages of one packet
        # each concatenate into the trace.
        self.out.write(pb.Trace(packet=[packet]).SerializeToString())

    def track(self, name, parent=None, rank=None, explicit_children=False):
        uuid = self.next_uuid
        self.next_uuid += 1
        descriptor = pb.TrackDescriptor(uuid=uuid, name=name)
        if parent is not None:
            descriptor.parent_uuid = parent
        if explicit_children:
            descriptor.child_ordering = pb.TrackDescriptor.EXPLICIT
        if rank is not None:
            descriptor.sibling_order_rank = rank
        self._packet(pb.TracePacket(track_descriptor=descriptor))
        return uuid

    def event(self, ts, kind, track, name=None, args=(), flows=(), terminating=()):
        event = pb.TrackEvent(
            type=kind,
            track_uuid=track,
            debug_annotations=[pb.DebugAnnotation(name=key, string_value=value) for key, value in args],
            flow_ids=flows,
            terminating_flow_ids=terminating,
        )
        if name is not None:
            event.name = name
        self._packet(pb.TracePacket(timestamp=ts, track_event=event))


# Instructions whose last operand is an offset from their own PC, which capstone prints as the offset.
PC_RELATIVE = {
    'j', 'jal', 'c.j', 'c.jal', 'beq', 'bne', 'blt', 'bge', 'bltu', 'bgeu', 'beqz', 'bnez', 'blez', 'bgez', 'bltz',
    'bgtz', 'bgt', 'ble', 'bgtu', 'bleu', 'c.beqz', 'c.bnez',
}


class Disassembler:
    """RISC-V instructions as text, through capstone, for a core with an ISA string such as rv32i or rv64gc."""

    def __init__(self, isa):
        isa = isa.lower()
        if isa[:4] not in ('rv32', 'rv64'):
            raise ValueError(f'ISA string {isa!r} does not start with rv32 or rv64')
        xlen = int(isa[2:4])
        # Only the C extension changes how instructions decode, since capstone knows every other extension at once.
        self.compressed = 'c' in isa[4:].split('_')[0]
        mode = {32: capstone.CS_MODE_RISCV32, 64: capstone.CS_MODE_RISCV64}[xlen]
        self.md = capstone.Cs(capstone.CS_ARCH_RISCV, mode | (capstone.CS_MODE_RISCVC if self.compressed else 0))
        self.md.detail = True
        self.mask = (1 << xlen) - 1
        self.cache = {}

    def __call__(self, bits, pc):
        if (bits, pc) not in self.cache:
            # With the C extension, an instruction whose low two bits are not 11 is compressed, in the low half.
            size = 4 if bits & 3 == 3 or not self.compressed else 2
            insn = next(self.md.disasm((bits & ((1 << 8 * size) - 1)).to_bytes(size, 'little'), pc), None)
            if insn is None:
                text = f'unknown 0x{bits:0{2 * size}x}'
            elif insn.mnemonic in PC_RELATIVE:
                target = (pc + insn.operands[-1].imm) & self.mask
                registers = insn.op_str.rpartition(',')[0]
                text = f'{insn.mnemonic} {registers + ", " if registers else ""}0x{target:x}'
            else:
                text = f'{insn.mnemonic} {insn.op_str}'.rstrip()
            self.cache[bits, pc] = text
        return self.cache[bits, pc]


class Resources:
    """The tracks of one field's values, each with as many lanes as objects that hold the value at once."""

    def __init__(self, writer, parent, ranks, title):
        self.writer, self.parent, self.ranks, self.title = writer, parent, ranks, title
        self.lanes = {}  # value -> [occupant object ID or None, per lane]
        self.uuids = {}  # (value, lane) -> track UUID

    def take(self, value, obj):
        lanes = self.lanes.setdefault(value, [])
        lane = next((i for i, occupant in enumerate(lanes) if occupant is None), len(lanes))
        if lane == len(lanes):
            lanes.append(None)
            name = self.title(value) if lane == 0 else f'{self.title(value)} {lane + 1}'
            rank = self.ranks.get(value, len(self.ranks)) * 1024 + lane
            self.uuids[value, lane] = self.writer.track(name, parent=self.parent, rank=rank)
        lanes[lane] = obj
        return self.uuids[value, lane]

    def release(self, value, uuid):
        lane = next(lane for (v, lane), u in self.uuids.items() if v == value and u == uuid)
        self.lanes[value][lane] = None


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    view_path, manifest_path, trace_path, output_path = sys.argv[1:]
    view = json.load(open(view_path))
    trace = Trace(manifest_path, trace_path)
    obj_type, track_field, label_field = view['object'], view['tracks'], view['label']
    instants = {f'{obj_type}.{e}' for e in view.get('instants', [])}
    disassemble = view.get('disassemble')
    dasm = Disassembler(disassemble['isa']) if disassemble else None
    flows = {f'{obj_type}.{e}': field for e, field in view.get('flows', {}).items()}
    schemas = {e['name']: e for e in trace.manifest['events']}
    field_types = {f['name']: f for e in trace.manifest['events'] if e.get('subject') == obj_type for f in e['fields']}

    def text(field, value):
        f = field_types.get(field) or next(f for e in trace.manifest['events'] for f in e['fields'] if f['name'] == field)
        if f['type'] == 'Enum':
            return trace.enums[f['enum']].get(value, f'{f["enum"]}({value})')
        if f['type'] == 'Id':
            return f'{f["object"]}#{value}'
        if f['type'] == 'UInt' and f['width'] > 16:
            return f'0x{value:0{(f["width"] + 3) // 4}x}'
        return str(value)

    track_type = field_types[track_field]
    ranks = {}
    if track_type['type'] == 'Enum':
        enum = next(e for e in trace.manifest['enums'] if e['name'] == track_type['enum'])
        ranks = {value: i for i, value in enumerate(enum['values'].values())}

    with open(output_path, 'wb') as out:
        writer = Writer(out)
        parent = writer.track(f'{obj_type} {track_field}', explicit_children=True)
        resources = Resources(writer, parent, ranks, lambda value: text(track_field, value))
        objects = {}  # object ID -> {'value', 'track', 'args'}, while it holds a track
        created = {}  # object ID -> its creation arguments
        next_flow = 1
        cycle_events, current = [], None

        def finish_cycle(cycle, events):
            # Every event of a cycle sees the state before its clock edge; their changes start at the next cycle.
            nonlocal next_flow
            ends, begins = [], []
            for e in events:
                if not e.subject or e.subject[0] != obj_type:
                    continue
                obj = e.subject[1]
                schema = schemas[e.name]
                short = e.name.split('.', 1)[1]
                if e.name in instants and obj in objects:
                    args = [(k, text(k, v)) for k, v in e.fields.items()]
                    terminating = []
                    other = e.fields.get(flows[e.name]) if e.name in flows else None
                    if other is not None and other in objects:
                        writer.event(cycle, INSTANT, objects[other]['track'], f'{short} of {obj_type}#{obj}',
                                     flows=[next_flow])
                        terminating = [next_flow]
                        next_flow += 1
                    writer.event(cycle, INSTANT, objects[obj]['track'], short, args=args, terminating=terminating)
                if schema.get('lifecycle') == 'create':
                    created[obj] = [(k, text(k, v)) for k, v in e.fields.items() if k != track_field]
                    if dasm and disassemble['bits'] in e.fields and disassemble['pc'] in e.fields:
                        created[obj].append(('asm', dasm(e.fields[disassemble['bits']], e.fields[disassemble['pc']])))
                if schema.get('lifecycle') == 'destroy':
                    ends.append((obj, short))
                elif track_field in e.fields:
                    if obj in objects:
                        ends.append((obj, None))
                    begins.append((obj, e.fields[track_field]))
            for obj, how in ends:
                state = objects.pop(obj, None)
                if state:
                    writer.event(cycle + 1, SLICE_END, state['track'], args=[('end', how)] if how else ())
                    resources.release(state['value'], state['track'])
                if how:
                    created.pop(obj, None)
            for obj, value in begins:
                track = resources.take(value, obj)
                objects[obj] = {'value': value, 'track': track}
                args = [('object', f'{obj_type}#{obj}')] + created.get(obj, [])
                known = dict(created.get(obj, []))
                label = known.get(label_field, f'{obj_type}#{obj}')
                if 'asm' in known:
                    label = f'{label}  {known["asm"]}'
                writer.event(cycle + 1, SLICE_BEGIN, track, label, args=args)

        last = 0
        for e in trace.events():
            if e.cycle != current:
                if cycle_events:
                    finish_cycle(current, cycle_events)
                cycle_events, current = [], e.cycle
            cycle_events.append(e)
            last = e.cycle
        if cycle_events:
            finish_cycle(current, cycle_events)
        # Objects still live when the trace ends keep their last slice open to its end.
        for obj, state in objects.items():
            writer.event(last + 1, SLICE_END, state['track'], args=[('end', 'live at the end of the trace')])


if __name__ == '__main__':
    main()
