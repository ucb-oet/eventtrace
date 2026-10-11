package eventtrace.examples.stage1

import chisel3._
import chisel3.util.Cat
import circt.stage.ChiselStage
import eventtrace.{EventTrace, Utils}
import eventtrace.EventTrace.syntax._

/** Two sites share the name "rest", but `phase` is 2 bits at one and 3 bits at the other. */
class MismatchedFields extends Module {
  val a = IO(Input(UInt(4.W)))
  when (a(0)) {
    EventTrace.event("rest") { "phase" := a(2, 1) }
  }.otherwise {
    EventTrace.event("rest") { "phase" := a(3, 1) }
  }
}

/** `high` sits under a `when` inside the event, which would not govern it. */
class NestedWhen extends Module {
  val a = IO(Input(UInt(4.W)))
  when (a(0)) {
    EventTrace.event("nested") {
      "low" := a(1)
      when (a(2)) { "high" := a(3) }
    }
  }
}

/** Elaborated inside the event body of [[CaptureInChild]], so its capture would land in the parent's event. */
class Helper extends Module {
  val in = IO(Input(UInt(4.W)))
  "inner" := in
}

/** The event's sink is in this module, but `Helper` captures a signal of its own. */
class CaptureInChild extends Module {
  val a = IO(Input(UInt(4.W)))
  EventTrace.event("child") {
    val helper = Module(new Helper)
    helper.in := a
  }
}

/** Same width as [[Mode]], but a different enum. */
object Gear extends ChiselEnum {
  val Low, Mid, High = Value
}

/** A different enum that is also named `Mode`. */
object Shadow {
  object Mode extends ChiselEnum {
    val A, B = Value
  }
}

/** Two sites share the name "shift", but `gear` is a [[Mode]] at one and a [[Gear]] at the other. */
class MismatchedEnums extends Module {
  val a = IO(Input(Bool()))
  when (a) {
    EventTrace.event("shift") { "gear" := Mode.Slow }
  }.otherwise {
    EventTrace.event("shift") { "gear" := Gear.Mid }
  }
}

/** Captures two different enums that are both named `Mode`. */
class SameEnumName extends Module {
  EventTrace.event("modes") {
    "outer" := Mode.Slow
    "inner" := Shadow.Mode.B
  }
}

/** A 72-bit field cannot decode into a C++ scalar. */
class WideField extends Module {
  val a = IO(Input(UInt(8.W)))
  EventTrace.event("toowide") { "x" := Cat(Seq.fill(9)(a)) }
}

/** A wire without a declared width has no width until firtool infers one, so its layout is unknown at elaboration. */
class UnknownWidth extends Module {
  val a = IO(Input(UInt(8.W)))
  val w = Wire(UInt())
  w := a
  EventTrace.event("unsized") { "w" := w }
}

/** The edge's register would update only while `a(0)` holds. */
class EdgeInsideWhen extends Module {
  val a = IO(Input(UInt(4.W)))
  when (a(0)) {
    val rise = Utils.rising(a(1))
    when (rise) {
      EventTrace.event("inner") {}
    }
  }
}

/** The event's enable would evaluate the condition again, building a second register inside the `when`. */
class EdgeAsCondition extends Module {
  val a = IO(Input(UInt(4.W)))
  when (Utils.rising(a(1))) {
    EventTrace.event("edge") {}
  }
}

/** `cycle` is a key every record already has. */
class ReservedName extends Module {
  val a = IO(Input(UInt(4.W)))
  EventTrace.event("reserved") { "cycle" := a }
}

/** Passes only if each design fails to elaborate with its expected error. */
object Rejects extends App {
  EventTrace.enable()
  val cases = Seq[(String, () => RawModule, String)](
    ("mismatched namesakes", () => new MismatchedFields, "sites sharing an event name must declare exactly the same fields"),
    ("capture under a nested when", () => new NestedWhen, "is under a `when` nested inside"),
    ("capture in a child module", () => new CaptureInChild, "is in a different module from"),
    ("reserved field name", () => new ReservedName, "uses a reserved name"),
    ("namesakes with different enums", () => new MismatchedEnums, "declares [gear: Gear], but the site with the same name"),
    ("two enums with one name", () => new SameEnumName, "two different ChiselEnums are named Mode"),
    ("field wider than 64 bits", () => new WideField, "is 72 bits wide"),
    ("field of unknown width", () => new UnknownWidth, "has an unknown width"),
    ("edge built inside a when", () => new EdgeInsideWhen, "Utils.rising creates a register, so call it outside any `when`"),
    ("edge as an event's condition", () => new EdgeAsCondition, "compute the edge into a `val` before the `when`")
  )
  val failures = cases.filterNot { case (label, gen, expected) =>
    val error = try { ChiselStage.emitCHIRRTL(gen()); None } catch { case e: Throwable => Some(e) }
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
