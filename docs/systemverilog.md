# Annotating SystemVerilog

A SystemVerilog design is traced in two halves. The design calls a DPI function of its own at each point where
something happens, passing the values involved. Its C++ handles each call with `oet`, a C++ library with the same
model as the Chisel library: events, objects, collections, and queries, with the same timing and rules (see
[Writing annotations](annotations.md)). The library writes the trace and its manifest, which the reader and viewers
read as they read a Chisel design's.

The examples have SystemVerilog versions in their `sv/` directories. `examples/stage0/sv` is the smallest and
`examples/collections/sv` the most complete.

## The SystemVerilog half

`systemverilog/oet.svh` gives two macros, which do nothing unless the build defines `OET_ENABLE`:

- `` `OET(call) `` makes a call a statement. Without `OET_ENABLE`, it is a null statement.
- `` `OET_SETUP(clk) `` sets up a module that calls. It declares `oet_cycle`, the edges of `clk` since the simulation
  started. It also opens the trace from `+eventtrace=<path>`, with the recording window's `+eventtrace_start` and
  `+eventtrace_stop`, as Chisel's sinks do, and finishes it when the simulation ends. Every module that calls uses it
  once; only the first open counts. Without `OET_ENABLE`, it is nothing.

The design's own DPI imports go inside `` `ifdef OET_ENABLE ``, so that a build without it compiles nothing of the
trace and has no DPI symbols. Here is stage0's counter, which records a tick every eighth cycle:

```sv
`include "oet.svh"

module Counter (input logic clock, input logic reset, output logic [31:0] io_count);
`ifdef OET_ENABLE
  import "DPI-C" function void oet_tick(input longint cycle, input int unsigned count, input byte delta);
`endif
  `OET_SETUP(clock)

  logic [31:0] count;
  always_ff @(posedge clock) begin
    if (reset) begin
      count <= 32'hffffffe0;
    end
    else begin
      count <= count + 32'd1;
      if (count[2:0] == 3'd0) `OET(oet_tick(oet_cycle, count, byte'(count[7:0]) - 8'sd20))
    end
  end
  assign io_count = count;
endmodule
```

Each DPI function is the design's own, written in this shape:

1. `input longint cycle`, which is always `oet_cycle`;
2. an optional `input int index`, for calls that come from several places; see [Calls](#calls);
3. the values, each a DPI integral type of at most 64 bits, such as `bit`, `byte`, `int`, or `longint`, signed or
   `unsigned`.

A call goes in the branch of the `always_ff` where the event happens, so that the branch's conditions are the event's,
as the `when` conditions around a Chisel site are. It sees the values from before the clock edge, as a Chisel sink
does. For logic written in `always_comb`, a call goes in a block of its own, inside the `ifdef`:

```sv
`ifdef OET_ENABLE
  always @(posedge clk_i) if (!rst_i && vkill_valid) oet_vkill(oet_cycle, vkill.vseq_first);
`endif
```

SystemVerilog widens a narrower vector to a DPI argument, but Verilator warns about it (`WIDTHEXPAND`); a cast, as in
`8'(phase)`, says the widening is meant.

## The C++ half

The C++ is one file, compiled as C++20, that includes Verilator's prototypes of the imports, so that a DPI function
with the wrong signature fails to compile, and `oet.h`:

```cpp
#include "VTestTop__Dpi.h"
#include "oet.h"
```

It declares the enums, object types, collections, events, and calls, and defines the DPI functions. Each DPI
function queues a handler, which runs once the cycle is complete and does what a Chisel event block does.

### Values

A value of a field or an event is one of:

- `oet::Bool`; an object field may also be declared with `bool`;
- `oet::UInt<N>` or `oet::SInt<N>`, of 1 to 64 bits;
- an enum declared with `OET_ENUM`;
- `oet::Id<T>`, the ID of an object of type `T`.

Each converts from an integer, an enum only explicitly, and an `Id` also from an object. Each knows whether it was set:
one that was not compares false with everything, as an unset field does in Chisel's queries, and `!=` is true.

### Enums

```cpp
OET_ENUM(Stage,
         Decode,      // waiting in decode
         Execute,
         Writeback);
```

A variant may take a value, as in `Execute = 4`. A value from the hardware converts explicitly, as `Stage(raw)`. As
in Chisel, a value that is no variant is recorded as it is, and the reader shows it as `Stage(3)`; only a value wider
than the enum stops the run. The enum's width is that of its largest value. Its names and values are copied by hand
from the SystemVerilog enum, and nothing compares the two.

### Object types

An object type is a struct deriving from `oet::Object`, whose fields are `oet::Const`, `oet::SetOnce`,
`oet::Mutable`, or `oet::Parent`, as in Chisel. `OET_OBJECT`, after it, names the type in the trace and lists its
fields in declaration order:

```cpp
struct Instruction : oet::Object {
  oet::Const<oet::UInt<8>> pc;
  oet::Const<bool> odd;
  oet::Mutable<Stage> stage;
  oet::SetOnce<oet::UInt<8>> result;
};
OET_OBJECT(Instruction, "instruction", pc, odd, stage, result)

struct Request : oet::Object {
  oet::Parent<Instruction> insn;  // the instruction that issued it
};
OET_OBJECT(Request, "request", insn)
```

An object's ID is `o.id`. A field reads as its value from before the clock edge; `.get()` gives that value, as a
query's key needs. Objects cannot be copied, so a handle is a reference: `Instruction& i = ...`.

### Collections

```cpp
oet::Collection<Instruction> active{"active"};
```

A collection keeps its members in the order they joined. Its queries are those of Chisel: `where(pred)`,
`order_by(key)`, `only()`, `head()`, `last()`, `at(k)`, `size()`, and iteration. `where` and `order_by` give a view,
which takes the same queries:

```cpp
Instruction& in(Stage s) { return active.where([s](Instruction& m) { return m.stage == s; }).only(); }

for (Instruction& i : active.order_by([](Instruction& m) { return m.odd.get(); })) sample.emit(i, {.pass = 0});
```

A predicate or key is a C++ lambda, so a query's message cannot show its text: it names the view as
`active.where(...)`.

### Events

An event has a subject type, or `oet::none`, and the struct of values it carries beyond its subject's fields, or
none. Its declaration gives its name, its lifecycle (`oet::create`, `oet::complete`, or `oet::abort`), the subject
fields its records assign, as member pointers, and, with `oet::by<T>()`, the type of the object a record may name as
its cause:

```cpp
oet::Event<Instruction> decode{"decode", oet::create, &Instruction::pc, &Instruction::odd, &Instruction::stage};
oet::Event<Instruction> issue{"issue", &Instruction::stage};
oet::Event<Instruction> retire{"retire", oet::complete};
oet::Event<Instruction> squash{"squash", oet::abort, oet::by<Instruction>()};
oet::Event<Instruction> stall{"stall", oet::by<Instruction>()};

struct IdleValues { oet::UInt<8> count; };
OET_FIELDS(IdleValues, count)
oet::Event<oet::none, IdleValues> idle{"idle"};
```

As in Chisel, the event is named after its subject's type, as `instruction.decode`.

The struct's members are values, or `std::optional`s of them, which a record may leave out. `OET_FIELDS` lists every
member, which the compiler checks. A record passes the values as a designated initializer, which leaves out the
optional ones it does not carry. Rocket's decode, for one, would carry why a stalled instruction waits:

```cpp
struct DecodeValues { std::optional<StallCause> stall; };
OET_FIELDS(DecodeValues, stall)
oet::Event<Instruction, DecodeValues> decode{"decode", oet::create,
    &Instruction::pc, &Instruction::odd, &Instruction::stage, oet::by<Instruction>()};

decode.emit(i);                                       // stall left out
decode.emit(i, {.stall = cause}, oet::blocked(b));
```

A subject field that some records leave unassigned is listed as `oet::optional(&Packet::kind)`. A create event lists
every `const` field of its subject, and no other event lists one. Sites sharing an event share its declaration, so
they agree on its fields and lifecycle.

### Calls

A call is one DPI function's handler, with a name, the number of indices it takes, and the events it emits:

```cpp
oet::Call decode_call{"decode", {decode}};
oet::Call every3_call{"every3", {busy, crowd, idle}};
oet::Call lane_call{"lane", 2, {lane}};  // a module instantiated twice
```

A DPI function queues its handler with `oet::at`, giving the cycle and the call, with its index if it takes several:

```cpp
void oet_decode(long long cycle, unsigned char pc) {
  oet::at(cycle, decode_call, [=] {
    Instruction& i = decode.create();
    i.pc = pc;
    i.odd = (pc & 1) != 0;
    i.stage = Stage::Decode;
    active.add(i);
    decode.emit(i);
  });
}

void oet_lane(long long cycle, int index, unsigned char value) {
  oet::at(cycle, lane_call[index], [=] { lane.emit({.value = value}); });
}
```

An index tells apart the slots of a channel, or the instances of a module, which takes its index from a parameter
that its parent sets. One DPI function may also queue several calls.

Without `+eventtrace`, `oet::at` returns at once, but a build with `OET_ENABLE` still makes its DPI calls; a build
that should cost nothing leaves `OET_ENABLE` out.

### Handlers

A handler does what a Chisel event block does, with Chisel's timing: queries read the state from before the clock
edge, and an event's effects hold from the next cycle.

- `event.create()` creates the subject of a create event, whose handler then assigns its fields and emits the event
  on it.
- Assigning a field, as `i.stage = Stage::Execute`, and `add` and `remove` on a collection are effects. They wait on
  the object until an event is emitted on it, which carries the assigned values and applies the effects at the end of
  the cycle. Effects still waiting when the handler returns stop the run.
- `emit` writes the event's record. It checks that the record assigns each listed subject field, assigns no other,
  and sets each value that is not optional, and that every value fits its type.
- An `if` takes the place of `EventTrace.test` and `otherwise`, and a range-for over a view the place of `iter`.

```cpp
void oet_every3(long long cycle, unsigned char exe_pc, unsigned char count) {
  oet::at(cycle, every3_call, [=] {
    auto executing = active.where([=](Instruction& m) { return m.pc == exe_pc && m.stage == Stage::Execute; });
    if (executing.size() == 1) {
      busy.emit(executing.only());
      if (active.size() >= 3)
        for (Instruction& j : active) crowd.emit(j);
    } else {
      idle.emit({.count = count});
    }
  });
}
```

A call, with its index, fires at most once per cycle. A cycle's handlers run in the order their calls are declared,
and its records are written in site order.

### Blocked objects and causes

Whether a record finds its subject blocked, and by what, is chosen at `emit`:

```cpp
decode.emit(i);                          // not blocked
decode.emit(i, oet::blocked(b));         // blocked by b
decode.emit(i, oet::blocked());          // blocked, cause unknown
squash.emit(i, oet::by(br));             // aborted by br
```

An event that declares `oet::by<T>()` has the field `by`, an ID of type `T`, with the role `aborted_by` for an abort
event and `blocked_by` otherwise. A record that names no cause leaves it out. An abort event is never emitted blocked.

## Sites

Each (call, index, event) has its own site, numbered in the order the calls are declared. An event with a subject has
two: one that does not block, and after it one that does, which `emit` picks for a blocked record. So site IDs differ
from a Chisel build of the same design, and an event that never blocks has a blocking site that never fires.

Within one file, the calls are declared top to bottom. Across files, C++ leaves the order unspecified, and in practice
it follows the order the files are linked. So a design whose calls are spread over several files may number its sites,
and order a cycle's handlers, differently from one build to the next. Each trace still reads correctly with the
manifest written beside it. To keep the numbering the same across builds, keep all the calls in one file. Failing that,
keep every call that creates objects of one type in the same file, so that the type's object IDs stay the same.

## The manifest

When the trace opens, the library checks the declarations and writes the manifest beside the trace, as
`<path>.json`, in the layout of a Chisel build's. Its sites have no `instance` or `source`, since a C++ site has no
single place in the design, and every `by` field is `optional`. The trace's header carries its SHA-256, as for any
trace.

## What is checked

The compiler checks the types: a listed field belongs to the event's subject, `OET_FIELDS` lists every member of its
struct, a value's type, and the DPI functions' signatures against the imports.

When the trace opens, the library checks the declarations. Among the rules: names are identifiers, and fields take
no reserved name; an object type has at most 64 fields and one parent; enums with one name are the same enum; events
with one name share one declaration; create events list every `const` field, and others list none; a cause or
lifecycle needs a subject; a call has at least one index.

While it records, the library checks each handler: the checks of `emit` above; a field assigned outside a handler, a
`const` field outside its create event, or a field twice before an event; an event the running call does not declare;
an event other than its create on an object created in the same cycle; an index out of range; and the runtime's
checks, as for a Chisel build (see [What the runtime checks](annotations.md#what-the-runtime-checks)). A broken rule
stops the simulation with a message on standard error that names where it broke, by its event and site or its call,
and the cycle:

```
eventtrace: cell.poke at site 6 (call poke, cycle 1): only found 0 members of Misuse.a.where(...)
```

A check on a handler runs when the handler first runs, so a call that never fires is never checked.

## Building with Verilator

A traced build adds to its Verilator command, where `$EVENTTRACE` is this repository:

- `+define+OET_ENABLE -I$EVENTTRACE/systemverilog`, for the macros;
- `-CFLAGS "-std=c++20 -I$EVENTTRACE/systemverilog -I$EVENTTRACE/runtime/src"`, for the library and the runtime
  it builds on;
- the design's C++ file, `$EVENTTRACE/systemverilog/oet.cc`, and `$EVENTTRACE/runtime/src/eventtrace_runtime.cc`, as
  absolute paths, since the makefile Verilator generates runs in its own directory.

A run then records with `+eventtrace=<path>`, and the manifest is beside the trace:

```
./obj_dir/V<top> +eventtrace=run.trace
eventtrace dump run.trace.json run.trace
```

A build that does not trace leaves out `OET_ENABLE` and the C++ files. `examples/sv.mk` builds the examples both
ways: `make check-sv` checks the traced build's records with the example's `check.awk`, and `make check-sv-off`
checks that the other build compiles nothing of the trace.

## What differs from Chisel

- Widths are declared in C++, not taken from the hardware. A value wider than its field stops the run; a
  declaration wider than the hardware goes unnoticed.
- Enums are copied by hand from the SystemVerilog.
- The declarations are checked when the trace opens, and a handler when it first runs, rather than at elaboration.
- A query's message names its view without the predicate's text.
- A design has two files, tied together by its DPI functions.
- Every event with a subject has a blocking site, and every `by` is optional in the manifest.
- Collections are sets only: queues, ROBs, and maps are not in `oet` yet.
