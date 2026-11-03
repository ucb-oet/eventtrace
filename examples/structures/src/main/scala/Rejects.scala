/* Designs breaking a rule of queues, ROBs, or maps, each of which must fail to elaborate with its own error. */
package eventtrace.examples.structures

import chisel3._
import circt.stage.ChiselStage
import eventtrace.EventTrace

/** An object type that none of the fixture's collections holds. */
class Other extends EventTrace.ObjectType("other") {
  val v = const(UInt(4.W))
}

/** A design whose body runs inside one module with an 8-bit input `a`, a queue, a ROB with 4 entries and 8 keys, and
  * a map with 4 entries.
  */
class Holder(body: (UInt, EventTrace.Queue[Cell], EventTrace.Rob[Cell], EventTrace.MapCollection[Cell]) => Unit)
    extends Module {
  val a = IO(Input(UInt(8.W)))
  val q = EventTrace.queue(new Cell, entries = 4)
  val r = EventTrace.rob(new Cell, entries = 4, keys = 8)
  val m = EventTrace.map(new Cell, entries = 4)
  body(a, q, r, m)
}

/** Passes only if each design fails to elaborate with its expected error. */
object Rejects extends App {
  EventTrace.enable()
  private def design(
    body: (UInt, EventTrace.Queue[Cell], EventTrace.Rob[Cell], EventTrace.MapCollection[Cell]) => Unit
  ): () => RawModule = () => new Holder(body)

  val cases = Seq[(String, () => RawModule, String)](
    ("a queue without entries", design { (_, _, _, _) =>
      EventTrace.queue(new Cell, entries = 0)
    }, "has 0 entries; a queue has at least 1"),
    ("a ROB without entries", design { (_, _, _, _) =>
      EventTrace.rob(new Cell, entries = 0, keys = 4)
    }, "has 0 entries; a ROB has at least 1"),
    ("a ROB with fewer keys than entries", design { (_, _, _, _) =>
      EventTrace.rob(new Cell, entries = 8, keys = 4)
    }, "has 4 keys for 8 entries; it needs a key for each entry"),
    ("a map without entries", design { (_, _, _, _) =>
      EventTrace.map(new Cell, entries = 0)
    }, "has 0 entries; a map has at least 1"),
    ("a literal ROB key outside its keys", design { (_, _, r, _) =>
      EventTrace.create(new Cell, event = "r_far") { c => c.key := 0.U; r.add(9.U, c) }
    }, "r.add gives key 9, but r's keys are below 8"),
    ("a literal map key outside its entries", design { (_, _, _, m) =>
      m(4.U).event("r_far_slot") {}
    }, "m(key) gives key 4, but m's keys are below 4"),
    ("pop outside an event", design { (_, q, _, _) =>
      q.pop()
    }, "q.pop() is outside an event block"),
    ("pop in an event without a subject", design { (_, q, _, _) =>
      EventTrace.event("r_bare") { q.pop() }
    }, "q.pop() is in the event \"r_bare\", which has no subject and so no effects"),
    ("pop in a create block", design { (_, q, _, _) =>
      EventTrace.create(new Cell, event = "r_new") { c => c.key := 0.U; q.pop() }
    }, "q.pop() is in its subject's create block, and a new object is in no queue yet"),
    ("pop in an event on another type", design { (_, q, _, _) =>
      val o = EventTrace.collection(new Other).head
      o.event("r_other") { q.pop() }
    }, "q.pop() is in an event on an object of type other, but q holds cell"),
    ("clear whose block has two events", design { (_, q, _, _) =>
      q.clear { c => c.event("r_one") {}; c.event("r_two") {} }
    }, "q.clear's block declares 2 events; it must hold exactly one event on its member"),
    ("rollback whose block has no event", design { (a, _, r, _) =>
      r.rollback(a(2, 0)) { _ => }
    }, "r.rollback's block declares 0 events; it must hold exactly one event on its member"),
    ("clear inside an event block", design { (_, q, _, _) =>
      val h = q.head
      h.event("r_outer") { q.clear { c => c.event("r_inner") {} } }
    }, "q.clear is inside an event block"),
    ("rollback whose block has an event on another object", design { (a, q, r, _) =>
      r.rollback(a(2, 0)) { _ => q.head.event("r_stray") {} }
    }, "is inside an iter block, which may hold only events on its member")
  )

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
  if (failures.nonEmpty) throw new RuntimeException(s"${failures.length} rejection case(s) failed")
}
