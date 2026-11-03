<!-- EventTrace's front page: what it is, how the repository is laid out, and how to build and check it. -->

# EventTrace

EventTrace records hardware events from annotations in Chisel generators, such as each instruction's pass through a
pipeline, and turns the recordings into views.

You annotate a generator with events. An event can create an object, such as an instruction, which later events move
through stages and finally end, by completing its work or by aborting; collections and queries find the object an
event acts on. A config that enables tracing elaborates capture hardware for the annotations, and a simulation then
writes every event to a trace. The reader prints a trace, or converts it for [Perfetto](https://ui.perfetto.dev) or
the [illilouette](https://github.com/ucb-oet/illilouette) waterfall viewer. A config that does not enable tracing
elaborates no tracing hardware.

```scala
import eventtrace.EventTrace
import eventtrace.EventTrace.syntax._

object Stage extends ChiselEnum {
  val Decode  = Value
  val Execute = Value
}

class Instruction extends EventTrace.ObjectType("instruction") {
  val pc    = const(UInt(32.W))
  val stage = mutable(Stage())
}

// In the module:
val active = EventTrace.collection(new Instruction)
when (enterDecode) {
  EventTrace.create(new Instruction, event = "decode") { i => i.pc := pc; i.stage := Stage.Decode; active.add(i) }
}
when (retire) {
  val i = active.head
  i.event("retire") { active.remove(i); i.complete() }
}
```

A SystemVerilog design is traced through DPI calls to its own C++, which uses `oet`, a C++ library with the same
model; see [Annotating SystemVerilog](docs/systemverilog.md).

The [documentation](docs/README.md) covers writing annotations, annotating SystemVerilog, using EventTrace in
Chipyard, reading traces, and the trace and manifest formats.

## Layout

| Directory | Contents |
|---|---|
| `frontend/` | The Chisel library. It also ships the runtime's C++, which elaboration writes next to the RTL. |
| `runtime/` | The runtime's C++, in `src/`, which the Chisel and SystemVerilog flows share, and its tests, which need no simulator. |
| `systemverilog/` | `oet`, the C++ library for SystemVerilog designs; `oet.svh`, the macros a design calls it through; and the library's tests, which need no simulator. |
| `proto/eventtrace.proto` | The trace format. |
| `python/` | The standalone Python producer package and its interoperability tests. |
| `reader/` | The `eventtrace` command, in Rust: `dump`, `perfetto`, and `waterfall`. Also the view files for Rocket, Saturn, Sodor, and native NPU models. |
| `examples/` | Small designs that elaborate, simulate, and check their traces, and check the library's elaboration errors. Each has a SystemVerilog version in `sv/`. |
| `docs/` | The documentation. |

## Use in Chipyard

EventTrace is a submodule of Chipyard at `tools/eventtrace`, and Chipyard builds the frontend as its `eventtrace`
project. To trace a design, add `chipyard.config.WithEventTrace` to its config. Elaboration then also writes
`<long name>.eventtrace.json`, the manifest that describes the traced events, next to the `.dts`. A simulation run
with `+eventtrace=<path>` writes the trace to that path:

```sh
make -C sims/verilator CONFIG=TracedRocketConfig run-binary BINARY=<elf> EXTRA_SIM_FLAGS=+eventtrace=$PWD/run.eventtrace
```

The traced configs so far are `TracedRocketConfig` and `TracedMINV128D64RocketConfig` (Rocket with Saturn) in
Chipyard, and `TracedSodor5StageConfig` in riscv-sodor. Their annotations are on the `eventtrace` branches of the
[ucb-oet](https://github.com/ucb-oet) forks of rocket-chip, saturn-vectors, and riscv-sodor.

To read a trace, build the reader and pass it the manifest:

```sh
cargo build --release --manifest-path reader/Cargo.toml
reader/target/release/eventtrace dump <manifest> <trace>
reader/target/release/eventtrace waterfall reader/rocket/view.json <manifest> <trace> run.waterfall
reader/target/release/eventtrace perfetto reader/rocket/view.json <manifest> <trace> run.perfetto-trace
```

## Building and testing

Run these from this directory inside a Chipyard checkout, after sourcing Chipyard's `env.sh` (for firtool and
Verilator). Cargo must also be installed, for the reader.

```sh
make -C runtime test
make -C systemverilog test
make -C examples/collections check    # likewise stage0, stage1, objects, and structures
```

This directory's `build.sbt` builds the frontend and the examples on their own. It uses Chipyard's sbt launcher and
caches, and the same Chisel version as Chipyard: Chisel 6 by default, or Chisel 7 with `USE_CHISEL7` set.

## Native Python producers

`python/` is a dependency-free SDK for models that already own simulation time
and object identity. Python 3.11 or newer is supported. From an EventTrace
checkout, install it with:

```sh
python3 -m pip install ./python
```

For SDK development, use `python3 -m pip install -e ./python` instead. Installing
and using the package requires neither Chipyard nor the Chisel or Rust toolchains.
The Rust reader is used separately to convert recordings into views.

Declare metadata on the model's own class; decorated method bodies are declarations
and are replaced by recorders. They must not contain simulation behavior.

```python
from enum import IntEnum
from eventtrace import (
    Const, Identity, Lifecycle, Mutable, TracedObject, U32, U64,
    Writer, event, object_type,
)

class Stage(IntEnum):
    Fetch = 0
    Execute = 1

@object_type(name="instruction")
class Instruction(TracedObject):
    id: Identity[U64]
    pc: Const[U32]
    stage: Mutable[Stage]

    def __init__(self, id, pc, writer):
        self.id, self.pc = id, pc
        self.bind_trace(writer, scope="core")

    @event(lifecycle=Lifecycle.CREATE, stage=Stage.Fetch)
    def fetch(self, cycle: int): ...

    @event
    def issue(self, cycle: int, stage: Stage = Stage.Execute): ...

    @event(lifecycle=Lifecycle.COMPLETE)
    def complete(self, cycle: int): ...

with Writer("run.eventtrace", sources=[(Instruction, "core")]) as writer:
    i = Instruction(42, 0x100, writer)
    i.fetch(1)
    i.issue(2)
    i.complete(5)
```

The writer generates `run.eventtrace.json`, including every declared event even
when it never fires. Properties annotated with `Const[T]` are read only on create;
for example a `bits` property can return the model's instruction encoding.
`SetOnce[T]` and `Mutable[T]` describe object fields; the SDK does not maintain a
second object-state store or enforce lifetime/assignment history. The model is
responsible for ID uniqueness, lifecycle, and assignment semantics.

Calls supply the global cycle unchanged. Cycles must be nondecreasing; within a
cycle records are stably ordered by site. A site may emit repeatedly in a cycle.
The writer holds only the current cycle, not live instructions. `close()` emits
its final cycle and closes the file; `flush()` flushes completed cycles while
keeping the current cycle open. A `.gz` suffix enables gzip, and the reader
automatically detects it. Unbound objects, or objects bound to `None`, do not
record or extract constant properties.

Use `bool`, `U8/U16/U32/U64`, `I32/I64`, `Annotated[int, UInt(n)/SInt(n)]`, or
`IntEnum` for event fields. `Id[Instruction]` accepts either a model object or its
integer ID. Optional fields use `T | None`; `Role.PARENT`, `Role.BLOCKED_BY`, and
`Role.ABORTED_BY` can annotate references. Lifecycle declarations take `Lifecycle`
values. Integer ranges, required fields, enum types, scopes, and cycle order are
validated before adding an event.

`@event(blocks=True)` declares a stall start. Emit it once when an object begins
waiting, and again only if the cause changes. In the waterfall view, an actual
stage change or object termination closes the stall. Reassigning the same stage
does not clear it. Explicit blocking is independent of stage occupancy; long
execution is never inferred to be stalled. Waterfall output uses the existing
version-1 `held` prefixes; a stall starting partway through a stage introduces
another display segment with the same stage ID. Existing viewers need no update.
A blocking observation describes its own cycle; the stage-changing or ending
cycle makes progress and is not counted as held. Field assignments still take
effect from `cycle + 1`.

Subjectless component events use `TraceSource`. Annotating a value with
`Counter[T]` declares a Perfetto counter; the other arguments identify its track.
Register all source classes/scopes before opening the writer. The trace header
hashes the exact generated manifest bytes, so schema discovery during execution
is not supported.

`reader/npu/view.json` is a view for native models whose `instruction` objects
have a `location` stage field, a `pc` label, and a `fetch` create event. Convert
recordings with a matching manifest using:

```sh
reader/target/release/eventtrace perfetto reader/npu/view.json run.eventtrace.json run.eventtrace run.perfetto-trace
reader/target/release/eventtrace waterfall reader/npu/view.json run.eventtrace.json run.eventtrace run.waterfall
```

Run the SDK tests from the EventTrace checkout. Build the Rust reader and install
`protoc` to include wire/export interoperability checks; those checks are skipped
when their prerequisites are absent:

```sh
python3 -m unittest discover -s python/tests -v  # after installing the package
cargo build --release --manifest-path reader/Cargo.toml
PYTHONPATH=python EVENTTRACE_BIN=$PWD/reader/target/release/eventtrace python3 -m unittest discover -s python/tests -v
```
