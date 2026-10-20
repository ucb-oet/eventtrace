# Writing annotations

Annotations are ordinary Chisel code in a module. They use these imports:

```scala
import eventtrace.EventTrace
import eventtrace.EventTrace.syntax._  // "name" := value, and Absent
import eventtrace.Utils                // only for the edge helpers
```

Each annotation is a *site*. A site emits its event on every clock edge at which the `when` conditions around it hold,
as a `printf` would. Like a `printf`, it emits nothing while its module's reset is held.

## Events

`EventTrace.event` emits an event with no subject. Its block captures the event's fields:

```scala
when (fire) {
  EventTrace.event("tick") { "count" := count; "lane" := 0.U(1.W) }
}
```

`"name" := value` and `EventTrace.capture("name", value)` are the same. A field may hold:

- a `Bool`, `UInt`, or `SInt` of known width, at most 64 bits. Split anything wider into several fields;
- a `ChiselEnum` value; see [Enums](#enums);
- an object's ID: `"source" := handle`, or `"source" := handle.id`. The trace shows it as `type#id`. To name the
  object that aborts or blocks the subject, use `abort(by = ...)` or `blocked(by = ...)` instead; see
  [Ending objects](#ending-objects) and [Blocked objects](#blocked-objects).

A literal, such as `0.U(1.W)`, is not sent from the hardware: elaboration knows it, so it costs nothing in simulation.

Rules for a capture:

- It must be in the event's own module. A parent may capture its child's IO ports, which it can see, but not a value
  inside the child.
- It must not be under a `when` nested inside the event's block. Conditional fields are not supported, so use a `Mux`,
  or a separate event under its own `when`.
- Event and field names must be C++ identifiers that are not C++ keywords. Every record already has the keys `cycle`,
  `event_id`, `site_id`, and `subject`, so no field may use those names.
- One block may not capture the same name twice.

### Sites that share an event name

Several sites may emit the same event. Readers see one event, so the sites must declare exactly the same fields, in
any order, with the same types. A site with no value for a field declares it `Absent`, and the field is left out of
that site's records:

```scala
when (stalled) {
  EventTrace.create(new Instruction(pcBits), event = "decode") { i => ...; "stall" := cause }
}
when (issuing) {
  EventTrace.create(new Instruction(pcBits), event = "decode") { i => ...; "stall" := Absent }
}
```

An `Absent` field takes its type from the other sites; if every site leaves a field `Absent`, elaboration fails.
`Absent` is EventTrace's own word, because Chisel's `DontCare` means the opposite: any value the compiler chooses.

## Enums

A field may hold a `ChiselEnum` value. Readers then see the variant's name, such as `stage=Execute`. A value that is
not a variant, which a hardware register can hold, shows as `Stage(6)`.

- The enum must be a Scala `object`. The library finds it from the value's static type, such as `Stage.Type`, so
  write `"stage" := stageReg` as usual. A helper that is generic over enums must pass the evidence along, as in
  `def note[T <: EnumType](v: T)(implicit ev: eventtrace.EnumOf[T])`. A value widened to plain `EnumType` does not
  compile.
- The manifest and the generated C++ name an enum by its simple name, so two different enums in one design may not
  share a name. For example, Rocket's trace has a `StallCause`, so Saturn's is called `VuopStallCause`.
- Sites that share an event name must use the same enum for a field, not only the same width.

## Object types

An object type is a class that extends `EventTrace.ObjectType`. Its fields are `val`s, and their names come from the
`val` names:

```scala
class Instruction(pcBits: Int) extends EventTrace.ObjectType("instruction") {
  val pc    = const(UInt(pcBits.W))
  val bits  = const(UInt(32.W))
  val fetch = parent(new Fetch(pcBits))     // the fetch packet it came from
  val stage = mutable(Stage())
}
```

Each field has a property:

- `const`: set by the create event and never assigned again;
- `setOnce`: unset at first, then assigned at most once;
- `mutable`: unset at first, then assigned any number of times.

A field may be a `Bool`, `UInt`, `SInt`, or `ChiselEnum`, as for captures. A `const` field may also hold the ID of
another object, declared with `Id(new T)`. Readers can then follow it.

`parent(new T)` declares a `const` field holding the ID of the object this one came from, such as an instruction's
fetch packet. Viewers use it to place an object under its parent. A type has at most one parent field.

A type may take parameters, such as a width, but every instance in one design must declare the same fields with the
same types. A type has at most 64 fields. Its name follows the event-name rules, and may not be the name of an event
without a subject.

The generated C++ uses the name `enums`, so neither an object type nor an event without a subject may have it.

## Creating objects

`EventTrace.create` creates an object, under the event named after the type and `event`, such as
`instruction.decode`:

```scala
when (enterDecode) {
  EventTrace.create(new Instruction(pcBits), event = "decode") { i =>
    i.pc := pc
    i.bits := bits
    i.fetch := packet
    i.stage := Stage.Decode
    active.add(i)
  }
}
```

The block receives the new object's handle, which is valid only inside the block. The block must set every `const`
field; `Absent` does not count. It may also capture event fields and add the object to collections.

## Events on objects

`handle.event(name)` emits an event whose subject is the object the handle refers to. A handle comes from a query;
see [Queries](#queries). The event is named after the type, such as `instruction.issue`:

```scala
when (issue) {
  val i = active.where(_.stage === Stage.Decode).only
  i.event("issue") { i.stage := Stage.Execute; "unit" := unitId }
}
```

Inside the block, assignments to the subject's fields are effects: they are part of the event's record, and they
change the object from the next cycle. The rules for assigning a field:

- A value wider than the field is an error; truncate it explicitly, as in `x(7, 0)`. A narrower value is zero- or
  sign-extended.
- Mixing `UInt` and `SInt` is an error. `Bool` and a one-bit `UInt` convert both ways. An enum field takes only its
  own enum.
- A block may assign each field at most once, and may not also capture an event field of the same name.
- A `const` field may be assigned only in the create block. A `setOnce` field that already has a value may not be
  assigned again; the runtime checks this.
- `i.cause := Absent` leaves the stored value as it is. It does not use up a `setOnce` field.

## Ending objects

An object's life ends with one of two calls, which say how it ended:

- `handle.complete()`: the object finished its work and ends normally, as an instruction that retires.
- `handle.abort()`: the object ends before finishing its work, as an instruction that is squashed, flushed, or
  replayed. `handle.abort(by = other)` also names the object responsible, in the field `by`.

```scala
i.event("retire") { active.remove(i); i.complete() }
i.event("squash") { active.remove(i); i.abort(by = branch) }
```

Only the event's subject can end, and by the end of the cycle it must have left every collection, through a `remove`
in the same block or in an earlier cycle: no other event may have an effect on an object in the cycle it ends. No
event may both create and end its subject, and sites sharing an event name must agree on whether they create,
complete, or abort.

## Blocked objects

`handle.blocked()` says that the event finds its subject blocked: waiting, rather than making progress.
`handle.blocked(by = other)` also names the object it waits on, in the field `by`:

```scala
when (stallBegins) {
  waiting.event("stall") { waiting.blocked(by = blocker) }
}
when (memWaitBegins) {
  i.event("mem_wait") { i.blocked() }
}
```

`blocked` may also be called in a create block, for an object that is blocked from the cycle it is created.

## The by field

`abort(by = ...)` and `blocked(by = ...)` capture the field `by`, and give it a role: the object that aborted the
subject, or the object it is blocked by. Viewers draw these links differently from other fields. Sites that share an
event name must declare `by` as for any other field, so a site with no object to name writes `by = Absent`:

```scala
u.event("stall") { "cause" := cause; u.blocked(by = vuopWithTag(tag)) }
u.event("stall") { "cause" := cause; u.blocked(by = Absent) }
```

A site that neither aborts nor blocks its subject, but shares its event name with sites that do, declares the field
as a plain capture, `"by" := Absent`. Sites that give `by` a value, or a role, must give it the same role.

## Collections

`EventTrace.collection` declares a collection in the current module:

```scala
val active = EventTrace.collection(new Instruction(pcBits))
```

A collection keeps its members in the order they joined. `active.add(i)` and `active.remove(i)` are effects. Each
must be inside an event whose subject is `i`, and takes effect when the event fires.

Each elaborated declaration is its own collection: two instances of a module get two collections. To use one
collection in several modules, see [Sharing collections between modules](sharing-collections.md). Error messages
name a collection by its module's instance path and its `val` name, such as `Core.active`.

## Queries

A query finds the objects an event acts on. The runtime evaluates it against the state from before the clock edge.
There are three kinds of query operation:

- **Views** filter or order a collection's members, and return another view. A collection is itself a view, in join
  order.
- **Choosers** pick one member and return a handle that can be the subject of an event.
- **`iter`** emits an event for every member.

| Operation | Returns |
|---|---|
| `view.where(m => pred)` | The members for which `pred` holds, in the view's order. |
| `view.orderBy(m => m.field)` or `orderBy(_.id)` | The members ranked by one key, ascending; ties keep the view's order. A member whose key is unset is a runtime violation. |
| `view.head`, `view.last` | The first or last member; a runtime violation if there is none. |
| `view.at(k)` | Member `k`, counting from 0; a runtime violation if there are not that many. |
| `view.only` | The one member; a runtime violation if there are none or several. |
| `view.iter { m => ... }` | Runs its block once at elaboration, with `m` standing for every member. |

### Predicates

A predicate compares the member's fields with literals, hardware values, or other handles' fields, and the member's
`id` with another handle's `id`. It uses `=== =/= < <= > >=`, and combines comparisons with `&& || !`. A hardware
`Bool` may be the right-hand operand of `&&` or `||`. Fields that hold an ID cannot be compared yet.

```scala
active.where(i => i.stage === Stage.Execute && i.pc === exePc).only
active.where(_.id > branch.id).iter { i => ... }
packets.where(p => p.stage =/= FetchStage.Buffer || rvc).head
```

A hardware value in a query is sent in the payload of every site that uses the query, so it counts as a capture of
those sites: it must be in the event's module. Two IDs can only be compared if their objects have the same type. An
unset field equals nothing, as a floating-point NaN does: `===` and the ordering comparisons are false when either
side is unset, and `=/=` is true.

A predicate may not refer to a create block's handle, since that object does not exist before the edge. It may not
refer to the member of another `where` either: queries do not nest.

### Where a handle may be used

- A create block's handle is valid inside its block only.
- A chooser's handle is valid anywhere, including in later queries.
- An `iter` member is valid inside its block only.
- The member that a `where` or `orderBy` passes to its function is valid inside that function only.

### iter

`view.iter` runs its block once at elaboration, with a handle that stands for every member. Each event on that handle
emits one record per member, in the view's order, and nothing for an empty view. The sink still calls the runtime
once, and the runtime loops over the members:

```scala
when (take_pc_mem) {
  val branch = active.where(_.stage === Stage.Memory).only
  active.where(i => i.stage === Stage.Execute || i.stage === Stage.Decode).iter { i =>
    i.event("squash") { active.remove(i); i.abort(by = branch) }
  }
}
```

An `iter` block holds only events on its member. It may not be nested in another `iter` block or placed inside an
event block.

### Tests on a view's size

`EventTrace.test` keeps the records of the sites inside it only when a comparison of a view's size holds, and
`otherwise` keeps its sites' records only when the comparison fails:

```scala
when (wb_valid) {
  val writeback = active.where(_.stage === Stage.Writeback)
  EventTrace.test(writeback.size === 1) {
    val i = writeback.only
    i.event("retire") { active.remove(i); i.complete() }
  }.otherwise {
    EventTrace.event("idle") { "count" := count }
  }
}
```

The runtime evaluates the test, so the hardware still sends the record whenever the Chisel conditions hold, and the
runtime drops it if the test fails. A dropped record takes no event ID. A test's block may hold queries, events,
`iter` blocks, nested tests, and Chisel `when`s around events. A Chisel connection inside a test is not gated by it,
so it does not belong there. A size can only be compared, with `===` or another comparison, against a Scala `Int`
that is at least 0, and only inside a test. `==` is an error, because it would run at elaboration, before anything has
been counted.

## Edge helpers

`Utils.rising(c)` is high in the first cycle of each run of `c`, and `Utils.falling(c)` in the first cycle after
one. They turn a condition that lasts several cycles into one event:

```scala
val stallBegins = Utils.rising(stall)
when (stallBegins) {
  waiting.event("stall") { waiting.blocked(by = blocker) }
}
```

Each creates a register, so compute it into a `val` outside any `when`. An event inside a `when` evaluates the
`when`'s condition again, which would build a second register, so the helpers refuse to run there.

## What elaboration checks

Elaboration stops with an error that names the source line when an annotation breaks a rule. Among the rules:

- names, widths, and types, as above;
- every `const` field is set by the create block, and no field is assigned twice in one block;
- a block never captures and assigns the same name;
- sites that share an event name have the same fields and types, give a field the same role, and agree on creating,
  completing, and aborting;
- effects act only on the event's subject;
- handles are used only where they are valid;
- events, `iter`, and tests do not nest in ways the library does not support;
- captures and query values are in the event's module;
- object types with one name have the same fields, and enums with one simple name are the same enum.

## What the runtime checks

Some rules depend on the simulation, so the runtime checks them as it handles each cycle:

- a chooser finds the members it needs;
- an object joins a collection only if it is not already a member, and leaves only if it is one;
- a `setOnce` field is assigned only once;
- no field is assigned by two events in one cycle;
- an object that ends has no other effects in its last cycle, and has left every collection;
- a site fires at most once per cycle.

The runtime checks these rules only while it records, that is, when the simulation was given `+eventtrace=<path>`.
A broken rule stops the simulation with a message on standard error that names the event, the site, and the cycle:

```
eventtrace: cell.poke at site 3 (cycle 1): only found 0 members of Misuse.a matching key == 0
```

The trace keeps every cycle before that one.

## Builds that do not trace

Tracing is off until the flow turns it on, as Chipyard's `WithEventTrace` does. With tracing off, no sinks are built,
and the blocks of events, creates, `iter`, `EventTrace.test`, and `otherwise` are not even run, so mistakes inside them
only surface in traced builds. Collections, object types, and queries outside those blocks are still declared and
checked.

Hardware that an annotation builds outside its event blocks, such as a cause computed with a `PriorityMux` or an
edge register, is still elaborated in a build that does not trace. Since nothing uses it there, firtool removes it.
If such hardware repeats an expression the design already computes, firtool may merge the two and keep the
annotation's name for the result, renaming the design's wire. To avoid this, use the design's own signal rather than
a copy of its expression.
