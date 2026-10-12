#!/usr/bin/env python3
"""Reads EventTrace traces: the protobuf stream that proto/eventtrace.proto describes, with the design's manifest.

As a program, prints a trace as text, one line per event, with absent fields left out:

  uv run --project tools/eventtrace/scripts tools/eventtrace/scripts/eventtrace_trace.py <manifest> <trace>

  eventtrace cycle=3 event_id=0 site_id=0 instruction.decode subject=instruction#0 odd=0 pc=0 stage=Decode

As a module, Trace(manifest, trace).events() yields each Event with its fields decoded, for other tools.

It parses records with the protobuf package, compiling eventtrace.proto with the protoc that grpcio-tools bundles
each time it starts, so no generated code is kept. uv installs both, as this directory's pyproject.toml lists them.
"""
import hashlib
import json
import os
import sys
import tempfile
from dataclasses import dataclass
from typing import Dict, Iterator, Optional, Tuple

from google.protobuf import descriptor_pb2, descriptor_pool, message_factory
from grpc_tools import protoc

FORMAT_VERSION = 1


@dataclass
class Event:
    cycle: int
    event_id: int
    site_id: int
    name: str
    # The subject as (object type, object ID), for an event with a subject.
    subject: Optional[Tuple[str, int]]
    # Present fields in the manifest's order: an int for a UInt, SInt, or Bool; an enum's numeric value; an object ID.
    fields: Dict[str, int]


PROTO = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'proto', 'eventtrace.proto')


def _record_class():
    """The Record message of eventtrace.proto, compiled now."""
    with tempfile.TemporaryDirectory() as tmp:
        descriptors = os.path.join(tmp, 'eventtrace.desc')
        args = ['protoc', f'--proto_path={os.path.dirname(PROTO)}', f'--descriptor_set_out={descriptors}',
                os.path.basename(PROTO)]
        if protoc.main(args) != 0:
            raise RuntimeError(f'protoc cannot compile {PROTO}')
        with open(descriptors, 'rb') as f:
            files = descriptor_pb2.FileDescriptorSet.FromString(f.read())
    pool = descriptor_pool.DescriptorPool()
    for file in files.file:
        pool.Add(file)
    return message_factory.GetMessageClass(pool.FindMessageTypeByName('eventtrace.Record'))


Record = _record_class()


def _varint(buf, pos):
    """A varint at `pos`, and the position after it. The protobuf package reads single messages but not a stream's
    length prefixes."""
    value = shift = 0
    while True:
        byte = buf[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, pos
        shift += 7


class Trace:
    def __init__(self, manifest_path, trace_path):
        manifest_bytes = open(manifest_path, 'rb').read()
        self.manifest = json.loads(manifest_bytes)
        self.manifest_sha256 = hashlib.sha256(manifest_bytes).hexdigest()
        self.events_by_name = {e['name']: e for e in self.manifest['events']}
        self.site_events = {s['site_id']: self.events_by_name[s['event_name']] for s in self.manifest['sites']}
        self.enums = {e['name']: {v: k for k, v in e['values'].items()} for e in self.manifest['enums']}
        self.trace_path = trace_path

    def _records(self):
        data = open(self.trace_path, 'rb').read()
        pos = 0
        while pos < len(data):
            size, pos = _varint(data, pos)
            yield data[pos:pos + size]
            pos += size

    def events(self) -> Iterator[Event]:
        records = self._records()
        first = Record.FromString(next(records, b''))
        if first.WhichOneof('record') != 'header':
            raise ValueError(f'{self.trace_path} does not start with a header')
        if first.header.version != FORMAT_VERSION:
            raise ValueError(f'{self.trace_path} has format version {first.header.version}; this reader knows {FORMAT_VERSION}')
        if first.header.manifest_sha256 != self.manifest_sha256:
            raise ValueError(f'{self.trace_path} was written by a design with another manifest')
        event_id = 0
        for data in records:
            record = Record.FromString(data)
            if record.WhichOneof('record') != 'cycle':
                continue
            for raw in record.cycle.events:
                yield self._event(record.cycle.cycle, event_id, raw)
                event_id += 1

    def _event(self, cycle, event_id, raw):
        schema = self.site_events[raw.site_id]
        subject = (schema['subject'], raw.subject) if 'subject' in schema else None
        fields, values = {}, iter(raw.values)
        for i, f in enumerate(schema['fields']):
            if raw.absent >> i & 1:
                continue
            value = next(values)
            # eventtrace.proto ZigZag-encodes an SInt in its uint64 values, as sint64 would.
            fields[f['name']] = (value >> 1) ^ -(value & 1) if f['type'] == 'SInt' else value
        return Event(cycle, event_id, raw.site_id, schema['name'], subject, fields)

    def text(self, event: Event) -> str:
        """The event as one line: its cycle, IDs, name, subject, and present fields, with enums by variant name."""
        parts = [f'eventtrace cycle={event.cycle} event_id={event.event_id} site_id={event.site_id} {event.name}']
        if event.subject:
            parts.append(f'subject={event.subject[0]}#{event.subject[1]}')
        for f in self.events_by_name[event.name]['fields']:
            if f['name'] not in event.fields:
                continue
            value = event.fields[f['name']]
            if f['type'] == 'Enum':
                value = self.enums[f['enum']].get(value, f'{f["enum"]}({value})')
            elif f['type'] == 'Id':
                value = f'{f["object"]}#{value}'
            parts.append(f'{f["name"]}={value}')
        return ' '.join(parts)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    trace = Trace(sys.argv[1], sys.argv[2])
    out = sys.stdout
    for event in trace.events():
        out.write(trace.text(event) + '\n')


if __name__ == '__main__':
    main()
