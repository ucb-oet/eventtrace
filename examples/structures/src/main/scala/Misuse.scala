/* Sites for the runtime checks of queues, ROBs, and maps, which violations.cc drives with synthetic records. */
package eventtrace.examples.structures

import chisel3._
import circt.stage.ChiselStage
import eventtrace.EventTrace

import java.nio.file.{Files, Paths}

/** An object held by the fixture's queue, ROB, and map. */
class Cell extends EventTrace.ObjectType("cell") {
  val key = const(UInt(4.W))
}

/** Sites for the runtime checks of the new kinds of collection, which `violations.cc` drives with synthetic records
  * rather than a simulation: each site elaborates fine, and only some sequences of records break a rule. Every key is
  * a literal, so no site has a payload.
  */
class Misuse extends Module {
  val fire = IO(Input(UInt(32.W)))

  val q = EventTrace.queue(new Cell, entries = 2)
  val r = EventTrace.rob(new Cell, entries = 2, keys = 4)
  val m = EventTrace.map(new Cell, entries = 2)

  when (fire(0)) { EventTrace.create(new Cell, event = "enq0") { c => c.key := 0.U; q.add(c) } }
  when (fire(1)) { EventTrace.create(new Cell, event = "enq1") { c => c.key := 1.U; q.add(c) } }
  when (fire(2)) { EventTrace.create(new Cell, event = "enq2") { c => c.key := 2.U; q.add(c) } }
  when (fire(3)) { val h = q.head; h.event("deq") { q.pop(); h.complete() } }
  when (fire(4)) { val t = q.last; t.event("deq_last") { q.pop(); t.complete() } }
  when (fire(5)) { q.clear { c => c.event("flush") { c.abort() } } }

  when (fire(6)) { EventTrace.create(new Cell, event = "alloc0") { c => c.key := 0.U; r.add(0.U, c) } }
  when (fire(7)) { EventTrace.create(new Cell, event = "alloc1") { c => c.key := 1.U; r.add(1.U, c) } }
  when (fire(8)) { EventTrace.create(new Cell, event = "alloc2") { c => c.key := 2.U; r.add(2.U, c) } }
  when (fire(9)) { EventTrace.create(new Cell, event = "alloc3") { c => c.key := 3.U; r.add(3.U, c) } }
  when (fire(10)) { r(1.U).event("find1") {} }
  when (fire(11)) { r.rollback(1.U) { c => c.event("squash1") { c.abort() } } }
  when (fire(12)) { r.rollback(3.U) { c => c.event("squash3") { c.abort() } } }
  when (fire(13)) { val h = r.head; h.event("commit") { r.pop(); h.complete() } }

  when (fire(14)) { EventTrace.create(new Cell, event = "put_a") { c => c.key := 0.U; m.add(1.U, c) } }
  when (fire(15)) { EventTrace.create(new Cell, event = "put_b") { c => c.key := 1.U; m.add(1.U, c) } }
  when (fire(16)) { val x = m(1.U); x.event("take") { m.remove(x); x.complete() } }
  when (fire(17)) { m(0.U).event("find0") {} }

  // A pop and a clear that keep the member alive, so that the only rule they break is removing it twice.
  when (fire(18)) { val h = q.head; h.event("deq_keep") { q.pop() } }
  when (fire(19)) { q.clear { c => c.event("drain") {} } }
}

object MisuseMain extends App {
  EventTrace.enable()
  val targetDir = args.headOption.getOrElse("build/misuse")
  ChiselStage.emitSystemVerilogFile(
    new Misuse,
    args = Array("--target-dir", targetDir, "--split-verilog"),
    firtoolOpts = Array("-disable-all-randomization", "-strip-debug-info")
  )
  Files.writeString(Paths.get(targetDir, "eventtrace_manifest.json"), EventTrace.manifest)
}
