"""A streaming, cycle-buffered encoder for proto/eventtrace.proto, version 1."""

from __future__ import annotations

import gzip
import hashlib
import json
from pathlib import Path

from .schema import U64, Lifecycle, TracedObject, field


def varint(value):
    """Encodes a non-negative integer as a protobuf varint."""
    out = bytearray()
    while value >= 128:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def integer(tag, value):
    """A varint field numbered `tag`, or nothing for 0, which proto3 leaves out."""
    return varint(tag << 3) + varint(value) if value else b""


def message(tag, data):
    """A length-delimited field numbered `tag`, holding `data`."""
    return varint(tag << 3 | 2) + varint(len(data)) + data


def record(tag, data):
    """A Record holding `data` as its field `tag`, preceded by its length."""
    payload = message(tag, data)
    return varint(len(payload)) + payload


class Writer:
    """Register all (source class, scope) pairs before opening a trace.

    TracedObject sources have model-owned IDs; other TraceSource instances emit
    subjectless events. No live-object or field-state store is maintained.
    """

    def __init__(self, path, *, sources, manifest_path=None, instruction_text=None):
        """Writes the manifest of `sources`, (class, scope) pairs, and opens the trace.

        The manifest goes to `manifest_path`, by default the trace's path without any
        .gz, plus .json; `instruction_text` goes into it as given. The trace, gzipped
        if `path` ends in .gz, starts with its header. Raises for a declaration it
        cannot record, such as a conflicting or duplicate one.
        """
        self.path = Path(path)
        plain = str(self.path).removesuffix(".gz")
        self.manifest_path = Path(manifest_path or plain + ".json")
        if self.path.absolute() == self.manifest_path.absolute():
            raise ValueError("trace and manifest paths must differ")
        self._sites = {}
        schemas, objects, enums, counters = {}, {}, {}, {}
        sites = []
        sources = list(sources)
        for cls, scope in sources:
            is_object = issubclass(cls, TracedObject)
            name = getattr(cls, "__eventtrace_name__", cls.__name__)
            if is_object:
                entry = {
                    "name": name,
                    "fields": [f.schema for f in cls.__eventtrace_fields__],
                }
                if name in objects and objects[name] != entry:
                    raise ValueError(f"conflicting object schema {name}")
                objects[name] = entry
            # Class definition order gives the site's ordering; instances share sites.
            methods = {}
            for base in reversed(cls.__mro__):
                methods.update(
                    {
                        n: v
                        for n, v in vars(base).items()
                        if hasattr(v, "__eventtrace_event__")
                    }
                )
            for method_name, method in methods.items():
                descriptor = method.__eventtrace_event__
                if descriptor.lifecycle and not is_object:
                    raise TypeError("lifecycle events require an object source")
                fields = descriptor.fields(cls)
                for i, f in enumerate(fields):
                    if f.optional and i >= 64:
                        raise ValueError("only the first 64 fields may be optional")
                    if f.reference and f.reference not in [c for c, _ in sources]:
                        raise ValueError(
                            f"unregistered referenced type {f.reference.__name__}"
                        )
                    if f.enum:
                        variants = {v.name: v.value for v in f.enum}
                        if (
                            f.enum.__name__ in enums
                            and enums[f.enum.__name__] != variants
                        ):
                            raise ValueError(f"conflicting enum {f.enum.__name__}")
                        enums[f.enum.__name__] = variants
                event_name = name + "." + method_name
                schema = {
                    "name": event_name,
                    "fields": [
                        {k: v for k, v in f.schema.items() if k != "property"}
                        for f in fields
                    ],
                }
                if is_object:
                    schema["subject"] = name
                if descriptor.lifecycle:
                    schema["lifecycle"] = descriptor.lifecycle.value
                if event_name in schemas and schemas[event_name] != schema:
                    raise ValueError(f"conflicting event schema {event_name}")
                schemas[event_name] = schema
                values = [f for f in fields if f.counter]
                if values:
                    if len(values) != 1 or is_object:
                        raise TypeError(
                            "a counter event has one Counter field and no subject"
                        )
                    counters[event_name] = {
                        "event": event_name,
                        "value": values[0].name,
                        "keys": [
                            n
                            for n in descriptor.signature.parameters
                            if n not in ("self", "cycle", values[0].name)
                        ],
                    }
                key = (cls, scope, descriptor)
                if key in self._sites:
                    raise ValueError(
                        f"duplicate source registration: {cls.__name__} {scope}"
                    )
                site_id = len(sites)
                self._sites[key] = site_id, fields
                code = descriptor.function.__code__
                site = {
                    "site_id": site_id,
                    "event_name": event_name,
                    "instance": scope,
                    "source": f"{code.co_filename}:{code.co_firstlineno}",
                }
                if descriptor.blocks:
                    site["blocks"] = True
                sites.append(site)
        # Object-only enums must be described even if no event currently carries them.
        for cls, _ in sources:
            for f in getattr(cls, "__eventtrace_fields__", []):
                if f.enum:
                    variants = {v.name: v.value for v in f.enum}
                    if f.enum.__name__ in enums and enums[f.enum.__name__] != variants:
                        raise ValueError(f"conflicting enum {f.enum.__name__}")
                    enums[f.enum.__name__] = variants
        self.manifest = {
            "events": [schemas[n] for n in sorted(schemas)],
            "objects": [objects[n] for n in sorted(objects)],
            "enums": [{"name": n, "values": enums[n]} for n in sorted(enums)],
            "sites": sites,
        }
        if counters:
            self.manifest["counters"] = list(counters.values())
        if instruction_text:
            self.manifest["instruction_text"] = instruction_text
        data = (json.dumps(self.manifest, indent=2, ensure_ascii=False) + "\n").encode()
        self.manifest_path.write_bytes(data)
        digest = hashlib.sha256(data).hexdigest().encode()
        self._out = (
            gzip.open(self.path, "wb")  # noqa: SIM115 -- Writer owns and closes the stream.
            if str(self.path).endswith(".gz")
            else self.path.open("wb")
        )
        try:
            self._out.write(record(1, integer(1, 1) + message(2, digest)))
        except BaseException:
            self._out.close()
            raise
        self._cycle = None
        self._events = []
        self._closed = False
        self._cycle_field = field("cycle", U64)

    def _emit(self, source, descriptor, cycle, values):
        """Encodes one event of `source` and buffers it with the events of its cycle.

        Cycles must not decrease; a later cycle writes out the buffered one first. The
        event is checked whole before anything is written.
        """
        if self._closed:
            raise ValueError("trace writer is closed")
        cycle = self._cycle_field.encode(cycle)
        if self._cycle is not None and cycle < self._cycle:
            raise ValueError(f"cycle {cycle} arrived after cycle {self._cycle}")
        key = type(source), getattr(source, "_eventtrace_scope", ""), descriptor
        if key not in self._sites:
            raise ValueError(
                f"unregistered trace source {type(source).__name__} in scope {key[1]!r}"
            )
        site, fields = self._sites[key]
        if descriptor.lifecycle is Lifecycle.CREATE:
            for f in source.__eventtrace_fields__:
                if f.schema.get("property") == "const":
                    values[f.name] = getattr(source, f.name)
        encoded, absent = bytearray(), 0
        for i, f in enumerate(fields):
            v = f.encode(values.get(f.name))
            if v is None:
                absent |= 1 << i
            else:
                encoded.extend(varint(v))
        subject = source.trace_identity() if isinstance(source, TracedObject) else 0
        data = integer(1, site) + integer(2, subject)
        if encoded:
            data += message(3, encoded)
        data += integer(4, absent)
        # Validate the whole event before advancing/flushing the stream.
        if cycle != self._cycle:
            self._flush_cycle()
            self._cycle = cycle
        self._events.append((site, message(2, data)))

    def _flush_cycle(self):
        """Writes the buffered events as one Cycle record, in site order."""
        if self._events:
            data = integer(1, self._cycle) + b"".join(
                data for _, data in sorted(self._events, key=lambda e: e[0])
            )
            self._out.write(record(2, data))
            self._events.clear()

    def flush(self):
        """Flush bytes already emitted; keep the current cycle open for more events."""
        if not self._closed:
            self._out.flush()

    def close(self):
        """Writes the last cycle and closes the trace; a second call does nothing."""
        if not self._closed:
            try:
                self._flush_cycle()
            finally:
                self._closed = True
                self._out.close()

    def __enter__(self):
        """Returns the writer, for a with block."""
        return self

    def __exit__(self, *_):
        """Closes the trace, whether or not the block raised."""
        self.close()
