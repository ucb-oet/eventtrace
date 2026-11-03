# Plan: tracing SystemVerilog designs

This plan records the agreed design for tracing SystemVerilog designs with EventTrace. Nothing in it is implemented
yet. The first design to trace is vroom's core3; which of its events to trace is a separate discussion.

## Approach

The Chisel frontend derives everything from elaboration: each site's sink, its payload layout, its C++ handler, and
the manifest. A SystemVerilog design has no elaboration step to hook into, so the work splits differently:

- **SystemVerilog** makes plain DPI calls, one per traced point, guarded by macros. Each call says that something
  happened and passes the values involved.
- **A C++ library** holds the semantics: object types, collections, queries, effects, lifecycles, and the timing
  rule. The person who traces a design writes the handlers against it, and they read much like the Chisel annotations.
- **The manifest is generated** from the C++ declarations, not written by hand.
- **The runtime, the trace and manifest formats, and the reader** are shared with the Chisel flow.

Some of Chisel's conveniences are given up; see [Accepted losses](#accepted-losses).

## Naming

New code uses `oet` and `OET`:

- SystemVerilog: the macros `` `OET(...) `` and `` `OET_CLOCK(clk) ``, the build define `OET_ENABLE`, and DPI
  functions named `oet_*`.
- C++: the namespace `oet`, and macros named `OET_*`.

The existing runtime keeps its `eventtrace` namespace and its `eventtrace_open`, `eventtrace_emit_w*` and
`eventtrace_finish` entry points, which the Chisel flow uses.

## The SystemVerilog side

The macros ship in a header under `systemverilog/`:

- `` `OET(call) `` expands to the call when `OET_ENABLE` is defined, and otherwise to a null statement, so that
  `` if (c) `OET(...) `` stays a statement.
- `` `OET_CLOCK(clk) `` declares `oet_cycle`, the clock edges since the simulation started. It also holds what a
  Chisel sink's `initial` and `final` blocks do: it reads the same plusargs (`+eventtrace`, `+eventtrace_start`,
  `+eventtrace_stop`), opens the trace, and finishes it.

**Cost when off is zero.** Without `OET_ENABLE`, nothing trace-related is compiled. The imports, `oet_cycle`, and the
`initial` and `final` blocks sit inside `` `ifdef OET_ENABLE ``, inline calls use `` `OET(...) ``, a block that
exists only to call sits inside `` `ifdef OET_ENABLE `` with the design's imports, and the build does not add the
C++ files. The simulated RTL is exactly the design's, with no DPI symbols. A build with `OET_ENABLE`
makes its DPI calls even when the run is not recording, so a run that should cost nothing is built without it.

**Each DPI function is written by the user,** in this shape:

1. `input longint cycle`;
2. an optional `input int index`;
3. the values, each `bit`, `int`, `int unsigned`, `longint` or `longint unsigned`, so at most 64 bits.

SystemVerilog converts narrower vectors and enums at the call, so C++ receives plain integers.

**A call goes in one of two places:**

- inside the `always_ff` branch where the event happens, whose conditions then act as Chisel's `when` stack;
- in its own `always @(posedge clk) if (!rst && cond)` block, for logic written in `always_comb`.

Either way the call sees the values from before the edge, as Chisel's sinks do.

**An index picks one of several sites.** The slots of a channel and the instances of a module pass an index. A module
instantiated more than once takes its index from a parameter its parent sets in its generate loop. C++ declares the
count. An index at or beyond it stops the run on its first call.

## The C++ library

The library lives in `runtime/`, beside the runtime it extends. It declares:

- enums, with `OET_ENUM`;
- object types, as structs deriving from `oet::Object` whose fields are `oet::Const<T>`, `oet::SetOnce<T>`,
  `oet::Mutable<T>` or `oet::Parent<T>`, registered with `OET_OBJECT`;
- collections, as `oet::Collection<T>`;
- events, as `oet::Event<T>`, with a name, a subject type, a lifecycle (`oet::create`, `oet::complete`, or
  `oet::abort`), the fields its records carry, and, as `oet::by<T>()`, the type of the object a record may name as
  what blocks or aborted its subject;
- calls, as `oet::Call`, each with a name, an optional count of indices, and the events its handler emits.

Collections and queries keep the Chisel frontend's operations: `where`, ordering, `head`, `last`, `at`, `only`,
iteration, and size. `EventTrace.test` and `otherwise` become `if` and `else`, and `iter` becomes a range-for over a
view. A field left `Absent` is unset: the manifest marks it `optional`, and an unset field compares false, as in
Chisel.

### Event fields

The manifest needs each event's fields before any handler runs, so each event lists them. Its subject's fields are
listed as pointers, which the compiler checks. The values it carries beyond them are the members of a struct,
registered with `OET_FIELDS` and given as the event's second template argument:

```cpp
oet::Event<Instruction> issue{"issue", &Instruction::stage};

struct DecodeValues { std::optional<StallCause> stall; };   // values beyond the subject's fields
OET_FIELDS(DecodeValues, stall)

oet::Event<Instruction, DecodeValues> decode{"decode", oet::create,
    &Instruction::pc, &Instruction::bits, &Instruction::stage, oet::by<Instruction>()};

decode.emit(i);                                       // stall absent
decode.emit(i, {.stall = cause}, oet::blocked(b));    // stall present
```

- A misspelled field or value fails to compile.
- Every record sets each listed subject field, except one listed as `oet::optional(&Packet::kind)`. `emit` checks
  this, and that the handler set no subject field the event does not list.
- A `std::optional` member may be left out, and the manifest marks it optional. The value types know whether they
  were set, so `emit` stops the run when a value that is not optional was never set.
- An event without a subject is emitted with only its values, as `idle.emit({.count = count})`.

### Calls and sites

A DPI function queues its handler with `oet::at(cycle, call[index], handler)`. One handler may emit several events,
so a call is not a site:

- The library makes the sites of each (call, index, event), numbered in declaration order: two for an event with a
  subject, as [Blocking and causes](#blocking-and-causes) explains, and one otherwise.
- Inside a handler, `event.emit(subject)` picks the site of the running call and index.
- Emitting an event that the running call did not declare stops the run.
- Sites of one event share its `oet::Event`, so they agree on its fields and lifecycle by construction.

A sketch, on vroom-shaped channels:

```cpp
oet::Event<VInsn> dispatch{"dispatch", oet::create, &VInsn::vseq, &VInsn::pc};
oet::Event<VInsn> kill    {"kill",     oet::abort};
oet::Event<VUop>  uop_kill{"kill",     oet::abort};

oet::Call vdisp_call{"vdisp", 2, {dispatch}};           // a dispatch site per slot
oet::Call vkill_call{"vkill",    {kill, uop_kill}};

void oet_vdisp(long long cycle, int slot, unsigned vseq, unsigned long long pc) {
    oet::at(cycle, vdisp_call[slot], [=] {
        VInsn& i = dispatch.create();
        i.vseq = vseq; i.pc = pc;
        vinsns.add(i);
        dispatch.emit(i);                               // the site of this slot
    });
}
```

### Blocking and causes

Whether a record is blocked, and by what, is chosen per record at `emit`. The declaration says only what the manifest
needs up front: with `oet::by<T>()`, the type of the object a record may name as its cause. The parentheses are
needed because `oet::by(br)` at `emit` shares the name, and C++ lets a function, but not a variable, share it.

```cpp
oet::Event<Instruction> decode{"decode", oet::create, &Instruction::pc, &Instruction::stage, oet::by<Instruction>()};
oet::Event<Instruction> squash{"squash", oet::abort, oet::by<Instruction>()};
oet::Event<Fetch>       miss  {"miss"};

decode.emit(i);                          // not blocked
decode.emit(i, oet::blocked(b));         // blocked by b
decode.emit(i, oet::blocked());          // blocked, cause unknown
squash.emit(i, oet::by(br));             // aborted by br
miss.emit(f, oet::blocked());            // no cause type declared, so blocked() only
```

The library maps this onto today's formats, which do not change:

- Each (call, index, event) with a subject gets two sites, one with `"blocks": true` and one without, and `emit` picks
  the right one.
- An event declared with `oet::by<T>()` has a field `by`, an `Id` of `T`, always `optional`. Its role is `aborted_by`
  for an `abort` event and `blocked_by` otherwise. A record that names no cause leaves it absent.
- An `abort` event emitted as blocked stops the run.
- `by` is not a field in the C++ API.

### Handler rules

These follow the Chisel frontend's rules.

1. **Timing.** Queries read the state from before the edge. Field assignments, `add`, `remove`, and endings are
   staged and hold from the next cycle. An assignment is also part of its event's record.
2. **Effects belong to an event on their object.** They accumulate on the object until an event on it is emitted.
   `emit` then takes the next event ID, checks the staged effects, checks that a `create` event set every `const`
   field, and writes the record. Effects still pending when the handler returns break a rule, as do effects outside
   a handler.
3. **A call, with its index, fires at most once per cycle.**
4. **Handlers run in the order their calls were declared,** and a cycle's records are written in site order.

### Where Chisel's elaboration checks end up

| Check | When it runs |
|---|---|
| Names are identifiers and not reserved; at most 64 fields; at most one parent; enum names are unique | At startup, when the trace opens |
| UInt, SInt, Bool, enum, and `Id` types | At compile time, through the wrapper types |
| An event's listed fields exist on its subject | At compile time |
| A handler sets exactly its event's fields | At `emit` |
| A value wider than its field | At `emit`, on the value |
| A `const` field assigned outside a `create` | At the assignment or at `emit` |
| Sites of one event agree on fields, roles, and lifecycle | By construction |
| Captures in the event's module, no `when` inside an event, handles used only where valid | Not applicable. A handle is a reference valid during its handler; keeping one longer is unchecked. |
| The existing runtime checks | Unchanged |

### Accepted losses

1. Widths are declared in C++, not taken from the hardware. A value wider than its field is caught; a declaration
   wider than the hardware is not.
2. Enum names and values are copied into `OET_ENUM` by hand, and nothing checks them against the SystemVerilog enum.
3. Checks on declarations run at startup, and checks on a handler's body run when it first fires, so a call that
   never fires is never checked. Chisel checks all of these at elaboration.
4. A query's error message cannot show its predicate's text, since a lambda is opaque.
5. A traced design has two files, tied together by DPI function names. A missing definition fails at link time, and a
   wrong signature fails at compile time when the C++ includes Verilator's DPI header.
6. The manifest marks `by` optional even where every record names a cause, which nothing reads today. Every event
   with a subject has a blocking site, which never fires for an event that never blocks, so the C++ flow's site IDs
   differ from Chisel's.

## The manifest

The library builds the manifest from the C++ declarations and writes it next to the trace, as `<trace>.json`, when
the run opens the trace. The trace's header carries its SHA-256, as the formats require.

A site entry lists only `site_id`, `event_name` and `blocks`. A C++ site has no single place in the source, so it has
no `instance` or `source`. Call names appear only in the runtime's error messages, such as
`vuop.rename at site 3 (call valloc[1], cycle 1200): only found 0 members of vinsns`.

## The runtime, shared with Chisel

- The runtime's C++ sources move from `frontend/src/main/resources/eventtrace/csrc/` to `runtime/`. The frontend's
  build packages them from there, so both flows ship the same files.
- Queued handlers become a second kind of record beside payload words. The Chisel path does not change.
- Without `+eventtrace`, the runtime ignores calls, as it does for Chisel's sinks today.
- The library needs C++20. The runtime sources it shares with the Chisel flow stay C++17, so the Chisel flow's
  builds are unaffected.

To contain the risk, the Chisel flow's generated files and traces must stay byte-identical before and after. That is
checked on the four examples and on Sodor's `rv32ui-p-add`, along with the runtime's own tests.

## The reader and formats

The reader makes a site's `instance` and `source` optional, and `docs/formats.md` says so. The waterfall writes an
empty text for a missing one, so illilouette needs no change. The trace format does not change.

## Documentation

A new page, "Annotating SystemVerilog", beside `docs/annotations.md`.

## Tests

- SystemVerilog and C++ versions of the `stage0`, `stage1`, `objects` and `collections` examples, checked by their
  `check.awk`, adjusted where the C++ flow's site IDs and its optional `by` differ from Chisel's.
- A version of the collections example's `Misuse` for the runtime's violations.
- C++ cases for the startup checks, where they replace the `Rejects` cases. Checks that become compile errors are not
  tested.
- The examples run on Chipyard's Verilator, 5.022. vroom needs 5.045 or newer.
