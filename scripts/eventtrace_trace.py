#!/usr/bin/env python3
"""Reads EventTrace traces: the protobuf stream that proto/eventtrace.proto describes, with the design's manifest.

As a program, prints a trace as text, one line per event, with absent fields left out:

  eventtrace_trace.py <manifest> <trace>

  eventtrace cycle=3 event_id=0 site_id=0 instruction.decode subject=instruction#0 odd=0 pc=0 stage=Decode

As a module, Trace(manifest, trace).events() yields each Event with its fields decoded, for other tools.

It decodes protobuf's wire format itself, since the format is small and the protobuf package is not always
installed; any protobuf library reads the same stream with eventtrace.proto.
"""
import hashlib
import json
import sys
from dataclasses import dataclass
from typing import Dict, Iterator, Optional, Tuple

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


def _varint(buf, pos):
    value = shift = 0
    while True:
        byte = buf[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, pos
        shift += 7


def _fields(message):
    """Yields (field number, value) for each field of a message: an int for a varint, bytes for a length-delimited."""
    pos = 0
    while pos < len(message):
        tag, pos = _varint(message, pos)
        number, wire_type = tag >> 3, tag & 7
        if wire_type == 0:
            value, pos = _varint(message, pos)
        elif wire_type == 2:
            size, pos = _varint(message, pos)
            value, pos = message[pos:pos + size], pos + size
        else:
            raise ValueError(f'unexpected wire type {wire_type} for field {number}')
        yield number, value


def _packed(data):
    pos = 0
    while pos < len(data):
        value, pos = _varint(data, pos)
        yield value


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
        first = dict(_fields(next(records, b'')))
        if 1 not in first:
            raise ValueError(f'{self.trace_path} does not start with a header')
        header = dict(_fields(first[1]))
        if header.get(1, 0) != FORMAT_VERSION:
            raise ValueError(f'{self.trace_path} has format version {header.get(1, 0)}; this reader knows {FORMAT_VERSION}')
        if header.get(2, b'').decode() != self.manifest_sha256:
            raise ValueError(f'{self.trace_path} was written by a design with another manifest')
        event_id = 0
        for record in records:
            for number, cycle_message in _fields(record):
                if number != 2:
                    continue
                cycle, raw_events = 0, []
                for field, value in _fields(cycle_message):
                    if field == 1:
                        cycle = value
                    elif field == 2:
                        raw_events.append(value)
                for raw in raw_events:
                    yield self._event(cycle, event_id, raw)
                    event_id += 1

    def _event(self, cycle, event_id, raw):
        site_id, subject_id, values, absent = 0, 0, [], 0
        for field, value in _fields(raw):
            if field == 1:
                site_id = value
            elif field == 2:
                subject_id = value
            elif field == 3:
                values = list(_packed(value))
            elif field == 4:
                absent = value
        schema = self.site_events[site_id]
        subject = (schema['subject'], subject_id) if 'subject' in schema else None
        fields, values = {}, iter(values)
        for i, f in enumerate(schema['fields']):
            if absent >> i & 1:
                continue
            value = next(values)
            fields[f['name']] = (value >> 1) ^ -(value & 1) if f['type'] == 'SInt' else value
        return Event(cycle, event_id, site_id, schema['name'], subject, fields)

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
