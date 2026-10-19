# Reading traces

The reader is the `eventtrace` command, in `reader/`, written in Rust. Build it with Cargo:

```sh
cargo build --release --manifest-path reader/Cargo.toml
```

Every subcommand takes the design's manifest and a trace, which may be a file or a named pipe. It refuses a trace whose
header names a different manifest.

| Command | Output |
|---|---|
| `eventtrace dump <manifest> <trace>` | Every event as a line of text, on standard output. |
| `eventtrace perfetto <view> <manifest> <trace> <output>` | A Perfetto trace, to open at https://ui.perfetto.dev. |
| `eventtrace waterfall <view> <manifest> <trace> <output>` | A waterfall file for the illilouette viewer. |

## Text output

`dump` prints one line per event: its cycle, its event and site IDs, its name, its subject, and its present fields,
sorted by name:

```
eventtrace cycle=399 event_id=0 site_id=1 instruction.decode subject=instruction#0 bits=0x00000517 pc=0x00010000 stage=Decode
```

Every command writes a value the same way:

- an enum value by its variant name, or as `Stage(6)` for a value that no variant has;
- an object ID as `type#id`;
- a `UInt` wider than 16 bits, such as an address, in hex padded to its width;
- any other number in decimal.

A field that a site leaves `Absent` is left out of the line.

## View files

The two converters need to know how to draw each object type, beyond what the manifest says. A view file gives this as
JSON: either one view, or a list of views, one per object type, in the order the viewer offers them. Rocket's,
`reader/rocket/view.json`, is:

```json
[
  {
    "object": "fetch",
    "tracks": "stage",
    "label": "pc",
    "hide": ["use"],
    "normal_end": "use",
    "created_in": {"lookup": "S1"}
  },
  {
    "object": "instruction",
    "tracks": "stage",
    "label": "pc",
    "hide": ["retire"],
    "normal_end": "retire",
    "disassemble": {"bits": "bits", "pc": "pc", "isa": "rv64gc"},
    "parent": "fetch",
    "created_in": {"decode": "Decode"}
  }
]
```

| Key | Meaning |
|---|---|
| `object` | The object type to draw. A single view may leave it out when the manifest has only one type. |
| `tracks` | The field whose value is the object's stage, or in Perfetto its track. An enum field's stages follow the order of its variants. |
| `label` | Optional. The field, or list of fields, that labels each object. By default, Perfetto labels an object by its ID. |
| `hide` | Optional. In Perfetto, the object's events that are neither marked nor draw arrows. |
| `normal_end` | Optional. In a waterfall, the event that ends an object normally, such as `retire`. An object ended by any other event is drawn as ending abnormally. Without it, every end is normal. |
| `disassemble` | Optional. The fields that hold a RISC-V instruction and its PC, and the core's ISA string, of which only XLEN and the C extension change decoding. Each object's label is followed by the instruction, written as `objdump -M no-aliases` writes it, with a PC-relative target made absolute. |
| `parent` | Optional. An Id field, set by the object's create event, naming the object it came from, of a type the file also draws. In a waterfall, an object's row follows its parent's. |
| `created_in` | Optional. For each create event of the object, the stage it is in during the cycle the event fires, as in `{"decode": "Decode"}`. A stage set at cycle c holds from c+1, so without this, an object's first cycle is drawn in no stage. |

`reader/rocket/`, `reader/saturn/`, and `reader/sodor/` hold the views of the traced designs.

## Perfetto

`perfetto` draws one track per value of each object type's `tracks` field, with a slice for each object while it
holds that value. Every field that an object's create event sets, except the track field, becomes an argument of its
slices.

An event that sets the track field begins a new slice. The object's other events, except its create event and those
that the view hides, are marked at their cycle on its track. An Id field names another object. If the view draws that
object's type, the field draws an arrow from a mark on that object's track to the event, for example from the blocker
to a stall.

Time is one nanosecond per cycle, so Perfetto's time axis reads in cycles. Objects that hold the same value at the
same time get separate lanes.

## Waterfall

`waterfall` writes one row per object, with its stages, label, how it ended, and its events. The rows are in parent
order: each object is followed by its children, the objects that name it as their parent. The file is illilouette's
`waterfall.proto`, of which `reader/proto/waterfall.proto` is a copy. Open it in illilouette.

A row's fields are the object's `const` fields, as its create event set them.
