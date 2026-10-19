package eventtrace.examples.objects

import chisel3._
import circt.stage.ChiselStage
import eventtrace.EventTrace
import eventtrace.EventTrace.syntax._

class Item(typeName: String) extends EventTrace.ObjectType(typeName) {
  val key   = const(UInt(4.W))
  val tag   = setOnce(UInt(4.W))
  val state = mutable(UInt(4.W))
  val flag  = mutable(Bool())
}

/** A design whose body runs inside one module with an 8-bit input `a` and a signed one `s`. */
class Case(body: (UInt, SInt) => Unit) extends Module {
  val a = IO(Input(UInt(8.W)))
  val s = IO(Input(SInt(4.W)))
  body(a, s)
}

/** A parameterized object type whose instances declare different fields. */
class Shape(withExtra: Boolean) extends EventTrace.ObjectType("shape") {
  val size = const(UInt(4.W))
  val extra = if (withExtra) Some(const(UInt(4.W))) else None
}

/** Passes only if each design fails with its expected error, during elaboration or when Chisel builds the generated
  * files at its end.
  */
object Rejects extends App {
  EventTrace.enable()
  private def design(body: (UInt, SInt) => Unit): () => RawModule = () => new Case(body)

  val cases = Seq[(String, () => RawModule, String)](
    ("const field not initialized", design { (a, _) =>
      EventTrace.create(new Item("r_unset"), event = "make") { p => p.tag := a(3, 0) }
    }, "does not initialize const field(s) key"),
    ("const field left Absent", design { (_, _) =>
      EventTrace.create(new Item("r_const_absent"), event = "make") { p => p.key := Absent }
    }, "does not initialize const field(s) key; Absent does not count"),
    ("const field assigned twice", design { (a, _) =>
      EventTrace.create(new Item("r_const2"), event = "make") { p => p.key := a(3, 0); p.key := a(7, 4) }
    }, "r_const2.key := assigns the field a second time in one block"),
    ("setOnce field assigned twice", design { (a, _) =>
      EventTrace.create(new Item("r_once2"), event = "make") { p => p.key := a(3, 0); p.tag := a(3, 0); p.tag := a(7, 4) }
    }, "r_once2.tag := assigns the field a second time in one block"),
    ("mutable field assigned twice", design { (a, _) =>
      EventTrace.create(new Item("r_mut2"), event = "make") { p => p.key := a(3, 0); p.state := a(3, 0); p.state := a(7, 4) }
    }, "r_mut2.state := assigns the field a second time in one block"),
    ("object field and event field with one name", design { (a, _) =>
      EventTrace.create(new Item("r_clash"), event = "make") { p => p.key := a(3, 0); "key" := a }
    }, "has the name of a field this block assigns to its subject"),
    ("namesake sites assigning different fields", design { (a, _) =>
      when (a(0)) { EventTrace.create(new Item("r_namesake"), event = "make") { p => p.key := a(3, 0); p.tag := a(7, 4) } }
      .otherwise  { EventTrace.create(new Item("r_namesake"), event = "make") { p => p.key := a(3, 0) } }
    }, "sites sharing an event name must declare exactly the same fields"),
    ("truncating assignment", design { (a, _) =>
      EventTrace.create(new Item("r_trunc"), event = "make") { p => p.key := a }
    }, "assigns a 8-bit value to a UInt<4> field; truncate it explicitly"),
    ("SInt into a UInt field", design { (_, s) =>
      EventTrace.create(new Item("r_mix"), event = "make") { p => p.key := s.asUInt; p.state := s }
    }, "assigns a SInt value to a UInt<4> field"),
    ("two bits into a Bool field", design { (a, _) =>
      EventTrace.create(new Item("r_bool"), event = "make") { p => p.key := a(3, 0); p.flag := a(1, 0) }
    }, "assigns a 2-bit value to a Bool field"),
    ("handle used after its block", design { (a, _) =>
      var escaped: Item = null
      EventTrace.create(new Item("r_escape"), event = "make") { p => p.key := a(3, 0); escaped = p }
      EventTrace.event("later") { escaped.state := a(3, 0) }
    }, "uses a handle after its create block closed"),
    ("event named like an object type", design { (a, _) =>
      EventTrace.create(new Item("r_named"), event = "make") { p => p.key := a(3, 0) }
      EventTrace.event("r_named") { "x" := a }
    }, "an object type is also named r_named"),
    ("object type instances with different fields", design { (a, _) =>
      EventTrace.create(new Shape(false), event = "make") { p => p.size := a(3, 0) }
      EventTrace.create(new Shape(true), event = "make") { p => p.size := a(3, 0); p.extra.get := a(7, 4) }
    }, "all instances of an object type must declare the same fields"),
    ("event field Absent at every site", design { (a, _) =>
      when (a(0)) { EventTrace.event("r_absent") { "cause" := Absent } }
      .otherwise  { EventTrace.event("r_absent") { "cause" := Absent } }
    }, "field \"cause\" of event \"r_absent\" is Absent at every site")
  )

  val failures = cases.filterNot { case (label, gen, expected) =>
    val error =
      try {
        ChiselStage.emitCHIRRTL(gen())
        None
      } catch { case e: Throwable => Some(e) }
    val messages = error.toSeq.flatMap { e =>
      Iterator.iterate[Throwable](e)(_.getCause).takeWhile(_ != null).flatMap(t => Option(t.getMessage)).toSeq
    }
    messages.find(_.contains(expected)) match {
      case Some(message) => println(s"PASS: $label rejected: $message"); true
      case None          => println(s"FAIL: $label: expected an error containing '$expected', got $error"); false
    }
  }
  if (failures.nonEmpty) throw new RuntimeException(s"${failures.length} rejection case(s) failed")
}
