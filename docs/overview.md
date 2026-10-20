# Overview

EventTrace records what happens inside a design during a simulation as a list of events, such as "instruction 12
moved to Execute" or "fetch packet 5 missed in the I-cache". You describe the events with annotations in the
generator's Chisel code. A traced simulation writes every event to a trace file, and the reader prints the trace or
converts it into views for [Perfetto](https://ui.perfetto.dev) or the
[illilouette](https://github.com/ucb-oet/illilouette) waterfall viewer.

## The model

- **Event.** A named record that an annotation emits on every clock edge at which its Chisel `when` conditions hold.
  An event has fields: values it captures, such as a cause or an address.
- **Site.** One annotation in the code. Several sites may emit the same event name, for example a retire in two
  different places; they then must declare the same fields.
- **Object.** Something with a lifetime, such as an instruction or a fetch packet. An object has a type, declared as
  a Scala class, and an ID that counts from 0 within its type. A create event starts its life, later events change
  its fields, and an event ends it, either normally, having completed its work, or by aborting it. An object's fields are `const` (set when it is created), `setOnce`, or
  `mutable`.
- **Subject.** The object an event is about. An event without a subject records values only.
- **Collection.** A set of live objects, in the order they joined it. Queries over a collection find the object an
  event acts on, such as "the instruction in Execute" or "the oldest instruction".
- **Effect.** A change an event makes to its subject: assigning a field, joining or leaving a collection, or ending
  its life.
- **Block.** An event may say that its subject is blocked, waiting rather than making progress, and by which object.
  Together with aborts, blocks are what viewers highlight.

A short example, from a pipeline:

```scala
val active = EventTrace.collection(new Instruction)

when (enter) {
  EventTrace.create(new Instruction, event = "decode") { i =>
    i.pc := count; i.stage := Stage.Decode
    active.add(i)
  }
}
when (issue) {
  val i = active.where(_.stage === Stage.Decode).only
  i.event("issue") { i.stage := Stage.Execute }
}
when (retire) {
  val i = active.head
  i.event("retire") { active.remove(i); i.complete() }
}
```

## Timing

An event at cycle c sees the state from before clock edge c. Its effects hold from cycle c+1. So a query at cycle c
does not see an object created at cycle c, and it sees a field's value from before any assignment at cycle c.

The events of one cycle happen together. The runtime handles them in site order, which is the order in which the
sites were elaborated, and numbers them in that order. That order is not causal: an object's ending event may
come before another of its events in the same cycle.

## How a trace is produced

1. **Elaboration.** With tracing enabled, each site becomes a capture sink: a small SystemVerilog block that calls the
   runtime through DPI. The sink's enable is the site's `when` conditions, and its payload is the hardware values
   the site captures. Constants and object references are not sent, because elaboration already knows them or the
   runtime works them out. Elaboration also generates C++ that describes every site and its queries. It also
   generates the manifest, a JSON file that names the events, their fields, the object types, and the enums.
2. **Simulation.** Each enabled sink hands its payload to the runtime, which is C++ compiled into the simulator. At
   the end of each cycle, the runtime evaluates the queries against the objects it tracks and checks the rules of
   the model. It numbers the events and new objects, applies the effects, and writes the cycle's events to the trace.
3. **Reading.** The reader takes the trace and the manifest, and prints the events or converts them for a viewer.

Queries run in the runtime, not in hardware. The hardware only sends what the sites capture, so tracking objects
adds no hardware beyond the sinks. How often a sink calls the runtime depends only on its `when` conditions.

A build that does not enable tracing builds no sinks and does not even run the annotations' blocks. Its Verilog is
the same as without the annotations, apart from source line numbers in messages and one case to avoid, described in
[Builds that do not trace](annotations.md#builds-that-do-not-trace).
