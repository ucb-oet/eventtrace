<!-- The contract for the trace and the manifest, for writing your own tools: the manifest's fields, the trace's
     records, what a trace means, and the optional metadata of native manifests. -->

# Trace and manifest formats

A trace is meaningful only with its design's manifest. The trace format, `proto/eventtrace.proto`, is the same for
every design. The manifest, written at elaboration, names what the trace's numbers refer to: each site's event, each
event's fields, the object types, and the enums.

To read traces from Rust, use the reader's library rather than parsing them yourself. It is the `eventtrace` crate in
`reader/`: `Manifest::load` reads a manifest, and `Events::open` reads a trace with it, one decoded event at a time.

## The manifest

The manifest is a JSON object with four lists.

`events`: one entry per event name, sorted by name:

```json
{
  "name": "fetch.lookup",
  "subject": "fetch",
  "lifecycle": "create",
  "fields": [
    {"name": "pc", "type": "UInt", "width": 40},
    {"name": "stage", "type": "Enum", "enum": "FetchStage", "width": 3}
  ]
}
```

- `subject` is the type of the event's subject, and is missing for an event without one.
- `lifecycle` is `create`, `complete` (the event ends its subject normally), or `abort` (it ends its subject before
  the subject finished its work), and is missing for any other event.
- `fields` are sorted by name. That is the order the trace's values follow. A field's `type` is `Bool`, `UInt`,
  `SInt`, `Enum` (with `enum`, the enum's name), or `Id` (with `object`, the type of the object it names, and no
  width). A field that some site leaves `Absent` has `"optional": true`. The field `by` of `abort` or `blocked` has
  `"role": "aborted_by"` or `"role": "blocked_by"`.

`objects`: one entry per object type, sorted by name, with its fields in declaration order. Each field also has its
`property`: `const`, `setOnce`, or `mutable`. A field declared with `parent` has `"role": "parent"`.

`enums`: each enum that some field uses, by its simple name, with its variants' values:

```json
{"name": "FetchStage", "values": {"S1": 0, "S2": 1, "Miss": 2, "Queue": 3, "Buffer": 4}}
```

`sites`: one line per site, mapping the site ID that every event carries to its event name, its module's instance
path, and its source location. `instance` and `source` are optional: a producer whose sites have no single place in
the design leaves them out. A site whose records find their subject blocked has `"blocks": true`:

```json
{"site_id": 0, "event_name": "fetch.lookup", "instance": "TestHarness.chiptop0.system.tile_prci_domain.element_reset_domain.rockettile.frontend", "source": "generators/rocket-chip/src/main/scala/rocket/Frontend.scala:399"}
```

`blocks: true` observes blocking during the event's own cycle `c`. Unlike an
object-field assignment, this observation is not delayed to `c+1`. Emit once on
entering a wait, or when its cause changes; repeated observations coalesce.
Blocking clears on an actual change of the view's stage field or on object
termination. Reassigning the same stage leaves it active. The cycle containing
the stage-changing or ending event is the progress/departure cycle and is not
counted as held; its field/lifetime effects still take effect from `c+1`.
For example, blocking observed at 3 and issue at 6 yields held cycles 3, 4, 5,
with the next stage starting at 7. No separate resume event is required. A wait
without a closing transition is drawn through the recorded window. This rule is
shared by all producers.

## The trace

A trace is a sequence of protobuf `Record` messages, each preceded by its length as a varint, as in protobuf's
delimited streams. The first record is a `Header`. Each later record is a `Cycle` that holds the events of one cycle
that had any.

- `Header`: the format's `version`, 1, and `manifest_sha256`, the SHA-256 of the manifest's text in hex. A reader
  should refuse a trace whose hash does not match its manifest.
- `Cycle`: the `cycle` number, which counts clock edges since the simulation started, and its `events`.
- `Event`:
  - `site_id`: the event's site, which the manifest's `sites` list maps to its event name.
  - `subject`: the ID of the subject, if the event has one. Its type is the event's `subject` in the manifest.
  - `values`: the values of the event's present fields, in the manifest's field order. A `UInt`, `Bool`, or enum is
    its value, and an `Id` is the object's ID. An `SInt` is its two's-complement value, ZigZag-encoded as protobuf's
    `sint64` is.
  - `absent`: a bit mask; bit i is set when field i is absent at this event's site.

Event IDs are not stored. Events are numbered from 0 in the order the trace holds them, through the whole trace.

## What a trace means

- An object is its type and ID. IDs are unsigned 64-bit values, unique within their object type for the recording. The Chisel runtime allocates them from 0; native models supply their own IDs, which may be sparse.
- An object lives from the event that the manifest marks as its `create` to the one marked as `complete` or `abort`,
  or to the end of the trace if it has none. Its events are those with it as their subject.
- An event at cycle c sees the state from before clock edge c. Its effects, such as creating or ending its
  subject or assigning a field, hold from cycle c+1.
- The events of one cycle happen together. Their order, by site ID, is not causal, so an object's ending event may
  come before another of its events in the same cycle.
- An event's fields include both the values its site captured and the values it assigned to its subject's fields.
- A trace may hold only a window of the simulation. Objects keep the IDs they had from the start of the simulation,
  and an object created before the window appears without its create event, so a reader only knows the fields that
  later events give.

## Optional native metadata

Native SDK manifests may additionally declare `counters` and `instruction_text`.
These are optional and do not change `eventtrace.proto` or its version.

```json
{
  "counters": [
    {"event": "RegisterTrace.write", "value": "value", "keys": ["regfile", "index"]}
  ],
  "instruction_text": {
    "instruction": {"field": "bits", "values": {"19": "addi x0, x0, 0"}}
  }
}
```

A counter is a subjectless numeric event. Its event, emitting scope, and ordered
key values identify a Perfetto counter track. Its value is plotted at the event's
cycle. Raw unsigned values retain all 64 bits; Perfetto counter plotting uses
floating point for values above signed 64-bit range.

Instruction text maps an unsigned field value (decimal JSON key) to a label for
one object type. The producer declares the table before recording, for example
from the loaded program. The converters append it to labels and Perfetto slice
arguments. Missing entries retain the numeric fields and use the configured
RISC-V decoder when available. Hardware manifests need neither extension.
