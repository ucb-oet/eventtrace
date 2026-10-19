# Sharing collections between modules

A collection belongs to the module that declares it. Each elaborated declaration is its own collection, so two
instances of a core get two collections. Often the events of one object are spread over several modules: in Rocket,
the frontend creates fetch packets, the instruction buffer consumes them, and the core kills them. Each of those
modules needs the same collection.

A collection has no hardware, so sharing it needs no wiring. Pass the Scala value to the modules that use it, in one
of two ways.

## As a constructor parameter

```scala
class Core extends Module {
  val active = EventTrace.collection(new Instruction)
  val decode = Module(new Decode(active))
  val retire = Module(new Retire(active))
}

class Decode(active: EventTrace.Collection[Instruction]) extends Module { ... }
```

This is the plainest way when you control the module's constructor.

## Through a Parameters key

In generators built on rocket-chip's Parameters, a key avoids changing a module's constructor. Define the key with a
default of `None`, so that a module built without the key still elaborates and simply traces nothing:

```scala
case object RocketTracePackets extends Field[Option[Collection[RocketTrace.Fetch]]](None)
```

Bind the key in the parent with `alterPartial`, where the parent builds the module. Every module inside it, at any
depth, inherits the binding:

```scala
val core = Module(new Rocket(outer)(outer.p.alterPartial {
  case RocketTracePackets => Some(outer.frontend.module.packets)
}))
```

Read the key in the module that uses it, and match on it:

```scala
p(RocketTracePackets) match {
  case Some(packets) =>
    when (io.imem.fire) {
      val head = packets.where(_.stage =/= FetchStage.Buffer).head
      head.event("take") { head.stage := FetchStage.Buffer }
    }
  case None =>
}
```

Never construct a collection inside a `Config`. Config lookups are evaluated again for each caller, so every module
would get its own collection.

## Elaboration order

A collection must be declared before the modules that use it elaborate. Declare it in a module that elaborates
before all of its users, and pass it down. In Rocket, the frontend elaborates before the core and the vector unit, so
the frontend declares both the fetch packets and the instructions.

The right-hand side of an `alterPartial` case is evaluated when the key is read, not when `alterPartial` is called. So
a binding may name a module that does not exist yet when the binding is made, as long as it exists by the time the
module that uses the key reads it. Rocket's tile binds the instructions for its vector unit this way:

```scala
val vector_unit = rocketParams.core.vector.map(v => LazyModule(v.build(p.alterPartial {
  case RocketTraceInstructions => Some(frontend.module.instructions)
})))
```

The tile builds the vector unit's LazyModule before the frontend's module exists. The vector unit only reads the key
while its own module elaborates, which happens after the frontend's module.

## Captures stay in the event's module

Sharing a collection does not share hardware. An event's captures, and the hardware values its queries compare with,
must be in the event's own module. A module that needs a signal from elsewhere gets it through its ports, as it would
for any other logic.
