# EventTrace

EventTrace records hardware events from annotations in Chisel generators, such as each instruction's pass through a
pipeline, and turns the recordings into views.

You annotate a generator with events. An event can create an object, such as an instruction, which later events move
through stages and finally destroy; collections and queries find the object an event acts on. A config that enables
tracing elaborates capture hardware for the annotations, and a simulation then writes every event to a trace. The
reader prints a trace, or converts it for [Perfetto](https://ui.perfetto.dev) or the
[illilouette](https://github.com/ucb-oet/illilouette) waterfall viewer. A config that does not enable tracing
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
  i.event("retire") { active.remove(i); i.destroy() }
}
```

## Layout

| Directory | Contents |
|---|---|
| `frontend/` | The Chisel library. It also ships the runtime's C++, which elaboration writes next to the RTL. |
| `runtime/` | Tests of the runtime, which need no simulator. |
| `proto/eventtrace.proto` | The trace format. |
| `reader/` | The `eventtrace` command, in Rust: `dump`, `perfetto`, and `waterfall`. Also the view files for Rocket, Saturn, and Sodor. |
| `examples/` | Small designs that elaborate, simulate, and check their traces, and check the library's elaboration errors. |

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
make -C examples/collections check    # likewise stage0, stage1, and objects
```

This directory's `build.sbt` builds the frontend and the examples on their own. It uses Chipyard's sbt launcher and
caches, and the same Chisel version as Chipyard: Chisel 6 by default, or Chisel 7 with `USE_CHISEL7` set.
