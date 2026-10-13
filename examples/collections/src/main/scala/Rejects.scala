package eventtrace.examples.collections

import chisel3._
import circt.stage.ChiselStage
import eventtrace.EventTrace
import eventtrace.EventTrace.syntax._

class Token extends EventTrace.ObjectType("token") {
  val slot = const(UInt(4.W))
}

/** A design whose body runs inside one module with an 8-bit input `a`, a signed one `s`, and a collection. */
class Case(body: (UInt, SInt, EventTrace.Collection[Instruction]) => Unit) extends Module {
  val a = IO(Input(UInt(8.W)))
  val s = IO(Input(SInt(4.W)))
  val active = EventTrace.collection(new Instruction)
  body(a, s, active)
}

/** A child module that builds a query on its own input, for a parent to use. */
class Chooser(active: EventTrace.Collection[Instruction]) extends Module {
  val pc = IO(Input(UInt(8.W)))
  val chosen = active.where(_.pc === pc).only
  val matching = active.where(_.pc === pc).size
}

/** Passes only if each design fails to elaborate with its expected error. */
object Rejects extends App {
  EventTrace.enable()
  private def design(body: (UInt, SInt, EventTrace.Collection[Instruction]) => Unit): () => RawModule =
    () => new Case(body)

  val cases = Seq[(String, () => RawModule, String)](
    ("effect on another object", design { (_, _, active) =>
      val a = active.head; val b = active.last
      a.event("r_other") { active.remove(b) }
    }, "remove acts on an object that is not the subject of the event \"instruction.r_other\""),
    ("field of another object", design { (_, _, active) =>
      val a = active.head; val b = active.last
      a.event("r_other_field") { b.stage := Stage.Decode }
    }, "instruction.stage := acts on an object that is not the subject"),
    ("effect in an event without a subject", design { (_, _, active) =>
      val a = active.head
      EventTrace.event("r_bare") { active.remove(a) }
    }, "which has no subject and so no effects"),
    ("destroy in a create block", design { (a, _, active) =>
      Instruction.create(event = "r_flash") { i => i.pc := a; i.odd := a(0); i.destroy() }
    }, "no event both creates and destroys its subject"),
    ("destroy twice", design { (_, _, active) =>
      val a = active.head
      a.event("r_twice") { active.remove(a); a.destroy(); a.destroy() }
    }, "destroys the subject a second time"),
    ("const field after creation", design { (a, _, active) =>
      val i = active.head
      i.event("r_const") { i.pc := a }
    }, "assigns a const field outside its object's create block"),
    ("effect under a nested when", design { (a, _, active) =>
      val i = active.head
      i.event("r_nested") { when (a(0)) { active.remove(i) } }
    }, "is under a `when` nested inside the event"),
    ("nested iter", design { (_, _, active) =>
      active.iter { _ => active.iter { j => j.event("r_inner") {} } }
    }, "iter is nested inside another iter block"),
    ("other event in an iter block", design { (a, _, active) =>
      active.iter { _ => EventTrace.event("r_aside") { "a" := a } }
    }, "inside an iter block, which may hold only events on its member"),
    ("iter member after its block", design { (_, _, active) =>
      var leaked: Instruction = null
      active.iter { i => leaked = i }
      leaked.event("r_leak") {}
    }, "uses an iter member after its iter block closed"),
    ("where member outside its predicate", design { (_, _, active) =>
      var leaked: Instruction = null
      active.where { i => leaked = i; i.pc === 0.U }
      leaked.event("r_member") {}
    }, "uses the member of a where or orderBy outside its predicate"),
    ("unbound handle", design { (_, _, _) =>
      (new Instruction).event("r_unbound") {}
    }, "uses an object that no create block or query produced"),
    ("create handle in a query", design { (a, _, active) =>
      Instruction.create(event = "r_young") { i =>
        i.pc := a; i.odd := a(0)
        "older" := active.where(_.id < i.id).last
      }
    }, "refers to a create block's subject"),
    ("nested query", design { (_, _, active) =>
      active.where(i => i.pc === active.where(j => j.pc === i.pc).only.pc)
    }, "refers to the member of another query"),
    ("query value from another module", design { (a, _, active) =>
      val child = Module(new Chooser(active))
      child.pc := a
      child.chosen.event("r_far") {}
    }, "uses a query comparing with a hardware value from another module"),
    ("SInt compared with a UInt field", design { (_, s, active) =>
      active.where(_.pc === s)
    }, "compares an SInt value with a UInt<8> field"),
    ("ids of different types", design { (_, _, active) =>
      val tokens = EventTrace.collection(new Token)
      active.where(_.id > tokens.head.id)
    }, "compares an ID of type instruction with an ID of type token"),
    ("negative position", design { (_, _, active) =>
      active.at(-1)
    }, "at(-1) needs a position of at least 0"),
    ("orderBy on another handle", design { (_, _, active) =>
      val a = active.head
      active.orderBy(_ => a.pc)
    }, "orderBy ranks by a field or the id of the member"),
    ("size compared with Scala's ==", design { (_, _, active) =>
      if (active.size == 1) active.head.event("r_eq") {}
    }, "compare it with === inside EventTrace.test"),
    ("negative size", design { (_, _, active) =>
      EventTrace.test(active.size > -1) {}
    }, "compares a view's size with -1; a size is at least 0"),
    ("test in an event block", design { (_, _, active) =>
      val a = active.head
      a.event("r_test_inside") { EventTrace.test(active.size === 1) {} }
    }, "EventTrace.test is inside an event block"),
    ("otherwise in an event block", design { (_, _, active) =>
      val a = active.head
      val t = EventTrace.test(active.size === 1) {}
      a.event("r_otherwise_inside") { t.otherwise {} }
    }, "otherwise is inside an event block"),
    ("test in an iter block", design { (_, _, active) =>
      active.iter { i => EventTrace.test(active.size === 1) { i.event("r_test_iter") {} } }
    }, "EventTrace.test is inside an iter block"),
    ("test value from another module", design { (a, _, active) =>
      val child = Module(new Chooser(active))
      child.pc := a
      EventTrace.test(child.matching === 1) { EventTrace.event("r_far_test") {} }
    }, "uses a query comparing with a hardware value from another module"),
    ("namesakes disagreeing on destroy", design { (_, _, active) =>
      val a = active.head; val b = active.last
      a.event("r_end") { active.remove(a); a.destroy() }
      b.event("r_end") {}
    }, "disagree on creating or destroying their subject"),
    ("ID field after creation", design { (_, _, active) =>
      val r = EventTrace.collection(new Request).head
      r.event("r_late") { r.insn := active.head }
    }, "request.insn := assigns a const field outside its object's create block"),
    ("ID field left Absent", design { (_, _, _) =>
      Request.create(event = "r_no_insn") { r => r.insn := Absent }
    }, "does not initialize const field(s) insn; Absent does not count"),
    ("ID of another type", design { (_, _, _) =>
      val other = EventTrace.collection(new Request).head
      Request.create(event = "r_wrong") { r => r.insn := other.id }
    }, "assigns the ID of an object of type request to a field holding IDs of type instruction")
  )

  val outside =
    try { EventTrace.collection(new Instruction); None }
    catch { case e: IllegalArgumentException => Some(e.getMessage) }
  val outsideOk = outside.exists(_.contains("is declared outside a module"))
  println(s"${if (outsideOk) "PASS" else "FAIL"}: collection outside a module rejected: $outside")

  val failures = cases.filterNot { case (label, gen, expected) =>
    val error =
      try { ChiselStage.emitCHIRRTL(gen()); None }
      catch { case e: Throwable => Some(e) }
    val messages = error.toSeq.flatMap { e =>
      Iterator.iterate[Throwable](e)(_.getCause).takeWhile(_ != null).flatMap(t => Option(t.getMessage)).toSeq
    }
    messages.find(_.contains(expected)) match {
      case Some(message) => println(s"PASS: $label rejected: $message"); true
      case None          => println(s"FAIL: $label: expected an error containing '$expected', got $error"); false
    }
  }
  if (failures.nonEmpty || !outsideOk)
    throw new RuntimeException(s"${failures.length + (if (outsideOk) 0 else 1)} rejection case(s) failed")
}
