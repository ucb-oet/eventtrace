# Using EventTrace in Chipyard

EventTrace is a submodule of Chipyard at `tools/eventtrace`, and Chipyard's `build.sbt` builds its frontend as the
`eventtrace` project. A generator that carries annotations depends on that project.

## Enabling tracing

Tracing is off unless a config adds `chipyard.config.WithEventTrace`:

```scala
class TracedRocketConfig extends Config(
  new chipyard.config.WithEventTrace ++
  new RocketConfig)
```

The harness turns tracing on before any chip elaborates. A config without `WithEventTrace` builds no tracing hardware,
so the generators can carry their annotations at no cost to anyone else.

These configs are traced so far:

| Config | Defined in | Traces |
|---|---|---|
| `TracedRocketConfig` | Chipyard | Rocket's instructions and fetch packets |
| `TracedMINV128D64RocketConfig` | Chipyard | The same, and the micro-ops of Saturn, the vector unit |
| `TracedSodor5StageConfig` | riscv-sodor | The five-stage Sodor core's instructions |

## Building

Build the simulator as usual:

```sh
make -C sims/verilator CONFIG=TracedRocketConfig
```

Elaboration writes the runtime's C++, the DPI adapter, and the generated `eventtrace_sites.h` next to the RTL, as it
does any black box's files, so the simulator build compiles them with nothing added.

It also writes the manifest, `<long name>.eventtrace.json`, next to the `.dts`, for example:

```
sims/verilator/generated-src/chipyard.harness.TestHarness.TracedRocketConfig/chipyard.harness.TestHarness.TracedRocketConfig.eventtrace.json
```

Keep the manifest with the traces you record: the reader needs it, and refuses a trace whose header names a
different manifest.

## Recording a trace

A simulation records only when it is given `+eventtrace=<path>`. Without it, the sinks still call the runtime, which
ignores them and checks no rules. With Chipyard's make targets, pass the plusarg through `EXTRA_SIM_FLAGS`, with an
absolute path, since the simulator runs in `sims/verilator`:

```sh
make -C sims/verilator CONFIG=TracedRocketConfig run-binary BINARY=<elf> \
  EXTRA_SIM_FLAGS=+eventtrace=$PWD/dhrystone.eventtrace
```

The path may also be a named pipe that the reader reads from as the simulation runs, so that a long run never stores
the whole trace:

```sh
mkfifo run.eventtrace
tools/eventtrace/reader/target/release/eventtrace waterfall <view> <manifest> run.eventtrace run.waterfall &
make -C sims/verilator ... EXTRA_SIM_FLAGS=+eventtrace=$PWD/run.eventtrace
```

### Recording a window

`+eventtrace_start=<trigger>` and `+eventtrace_stop=<trigger>` limit the trace to a window of the simulation. A
trigger is one of:

- `cycle:<n>`, which fires at cycle n;
- `event:<name>`, which fires at the first event of that name, as in `event:instruction.retire`;
- `event:<name>,<field>=<value>,...`, which fires at the first event of that name whose listed fields hold those
  values, as in `event:instruction.decode,pc=0x80000000` or `event:instruction.mark_replay,cause=cache_miss`.

A value is a number, in decimal or with `0x` in hex (a leading `0` reads it as octal), or a variant name for an enum
field. A trigger cannot match an event's subject ID.

The window opens at the cycle where the start trigger fires. It closes after the cycle where the stop trigger fires,
and a stop trigger only counts events from the cycle the window opened. Either trigger may be left out, to record
from the first cycle or to the last. The runtime still tracks every cycle, so objects keep the IDs they had from the
start of the simulation. An object created before the window appears without its create event.

If a trigger never fires, the runtime says so when the simulation ends.

## When the runtime stops a simulation

If the records break a rule of the model, such as `.only` finding two members, the runtime stops the simulation with
a message that names the event, the site, and the cycle; see
[What the runtime checks](annotations.md#what-the-runtime-checks). The trace keeps every cycle before that one.

The runtime writes its messages to standard error. Chipyard's `run-binary` sends the simulator's standard error
through `spike-dasm` into `<binary>.out` in its output directory, so look there for them, and for the message about a
trigger that never fired.

## Other flows

Outside Chipyard, a flow does what Chipyard's harness does:

1. Call `EventTrace.enable()` before elaborating the design.
2. Elaborate into a target directory, for example with `ChiselStage.emitSystemVerilogFile`, and pass firtool
   `--split-verilog`, as the examples do, so that it writes each file separately. With the RTL, firtool writes the
   sinks' SystemVerilog, the runtime's C++, and the generated `eventtrace_sites.h`.
3. Write `EventTrace.manifest` to a file after elaborating.
4. Build the simulator from every `.sv` and `.cc` file in the target directory, with the directory on the C++ include
   path.

The examples do exactly this with Verilator; see `examples/common.mk` and any example's `Main`.
