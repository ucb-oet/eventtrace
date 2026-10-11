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
class Items(typeName: String) extends EventTrace.Schema(new Item(typeName))

/** A design whose body runs inside one module with an 8-bit input `a` and a signed one `s`. */
class Case(body: (UInt, SInt) => Unit) extends Module {
  val a = IO(Input(UInt(8.W)))
  val s = IO(Input(SInt(4.W)))
  body(a, s)
}

/** A parameterized schema whose instances declare different fields. */
class Shape(withExtra: Boolean) extends EventTrace.ObjectType("shape") {
  val size = const(UInt(4.W))
  val extra = if (withExtra) Some(const(UInt(4.W))) else None
}
class Shapes(withExtra: Boolean) extends EventTrace.Schema(new Shape(withExtra))

/** Passes only if each design fails with its expected error, during elaboration or when Chisel builds the generated
  * files at its end.
  */
object Rejects extends App {
  EventTrace.enable()
  private def design(body: (UInt, SInt) => Unit): () => RawModule = () => new Case(body)

  val cases = Seq[(String, () => RawModule, String)](
    ("const field not initialized", design { (a, _) =>
      new Items("r_unset").create(event = "make") { p => p.tag := a(3, 0) }
    }, "does not initialize const field(s) key"),
    ("const field left DontCare", design { (_, _) =>
      new Items("r_dontcare").create(event = "make") { p => p.key := DontCare }
    }, "does not initialize const field(s) key; DontCare does not count"),
    ("const field assigned twice", design { (a, _) =>
      new Items("r_const2").create(event = "make") { p => p.key := a(3, 0); p.key := a(7, 4) }
    }, "r_const2.key := assigns the field a second time in one block"),
    ("setOnce field assigned twice", design { (a, _) =>
      new Items("r_once2").create(event = "make") { p => p.key := a(3, 0); p.tag := a(3, 0); p.tag := a(7, 4) }
    }, "r_once2.tag := assigns the field a second time in one block"),
    ("mutable field assigned twice", design { (a, _) =>
      new Items("r_mut2").create(event = "make") { p => p.key := a(3, 0); p.state := a(3, 0); p.state := a(7, 4) }
    }, "r_mut2.state := assigns the field a second time in one block"),
    ("object field and event field with one name", design { (a, _) =>
      new Items("r_clash").create(event = "make") { p => p.key := a(3, 0); "key" := a }
    }, "has the name of a field this block assigns to its subject"),
    ("namesake sites assigning different fields", design { (a, _) =>
      val items = new Items("r_namesake")
      when (a(0)) { items.create(event = "make") { p => p.key := a(3, 0); p.tag := a(7, 4) } }
      .otherwise  { items.create(event = "make") { p => p.key := a(3, 0) } }
    }, "sites sharing an event name must declare exactly the same fields"),
    ("truncating assignment", design { (a, _) =>
      new Items("r_trunc").create(event = "make") { p => p.key := a }
    }, "assigns a 8-bit value to a UInt<4> field; truncate it explicitly"),
    ("SInt into a UInt field", design { (_, s) =>
      new Items("r_mix").create(event = "make") { p => p.key := s.asUInt; p.state := s }
    }, "assigns a SInt value to a UInt<4> field"),
    ("two bits into a Bool field", design { (a, _) =>
      new Items("r_bool").create(event = "make") { p => p.key := a(3, 0); p.flag := a(1, 0) }
    }, "assigns a 2-bit value to a Bool field"),
    ("handle used after its block", design { (a, _) =>
      var escaped: Item = null
      new Items("r_escape").create(event = "make") { p => p.key := a(3, 0); escaped = p }
      EventTrace.event("later") { escaped.state := a(3, 0) }
    }, "uses a handle after its create block closed"),
    ("event named like an object type", design { (a, _) =>
      new Items("r_named").create(event = "make") { p => p.key := a(3, 0) }
      EventTrace.event("r_named") { "x" := a }
    }, "an object type is also named r_named"),
    ("object type instances with different fields", design { (a, _) =>
      new Shapes(false).create(event = "make") { p => p.size := a(3, 0) }
      new Shapes(true).create(event = "make") { p => p.size := a(3, 0); p.extra.get := a(7, 4) }
    }, "all instances of an object type must declare the same fields"),
    ("event field DontCare at every site", design { (a, _) =>
      when (a(0)) { EventTrace.event("r_absent") { "cause" := DontCare } }
      .otherwise  { EventTrace.event("r_absent") { "cause" := DontCare } }
    }, "field \"cause\" of event \"r_absent\" is DontCare at every site")
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
