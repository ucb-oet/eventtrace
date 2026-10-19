package eventtrace.examples.collections

import chisel3._
import circt.stage.ChiselStage
import eventtrace.EventTrace
import eventtrace.EventTrace.syntax._

import java.nio.file.{Files, Paths}

class Cell extends EventTrace.ObjectType("cell") {
  val key   = const(UInt(4.W))
  val once  = setOnce(UInt(4.W))
  val value = mutable(UInt(4.W))
}

/** An object type that no collection holds, so the runtime numbers its objects but does not keep them. */
class Note extends EventTrace.ObjectType("note") {
  val v = const(UInt(4.W))
}

/** Sites for the runtime's violation checks, which `violations.cc` drives with synthetic records rather than a
  * simulation: each fixture elaborates fine, and only some sequences of records break a rule. Every value is a
  * literal, so no site has a payload.
  */
class Misuse extends Module {
  val fire = IO(Input(UInt(32.W)))

  val a = EventTrace.collection(new Cell)
  val b = EventTrace.collection(new Cell)
  val zero = a.where(_.key === 0.U).only

  when (fire(0)) { EventTrace.create(new Cell, event = "make0") { c => c.key := 0.U; a.add(c) } }
  when (fire(1)) { EventTrace.create(new Cell, event = "make1") { c => c.key := 1.U; a.add(c) } }
  when (fire(2)) { zero.event("set_once") { zero.once := 1.U } }
  when (fire(3)) { zero.event("poke") { zero.value := 1.U } }
  when (fire(4)) { zero.event("poke_again") { zero.value := 2.U } }
  when (fire(5)) { zero.event("rejoin") { a.add(zero) } }
  when (fire(6)) { zero.event("join_b") { b.add(zero) } }
  when (fire(7)) { zero.event("join_b_again") { b.add(zero) } }
  when (fire(8)) { zero.event("leave_b") { b.remove(zero) } }
  when (fire(9)) { zero.event("leave_b_again") { b.remove(zero) } }
  when (fire(10)) { zero.event("retire") { a.remove(zero); zero.destroy() } }
  when (fire(11)) { zero.event("vanish") { zero.destroy() } }
  when (fire(12)) { zero.event("poke_late") { zero.value := 3.U } }
  when (fire(13)) { b.head.event("first_b") {} }
  when (fire(14)) { a.at(2).event("third") {} }
  when (fire(15)) { a.where(_.key === 3.U).only.event("three") {} }
  when (fire(16)) { a.where(_.key < 2.U).only.event("small") {} }
  when (fire(17)) { a.orderBy(_.once).head.event("by_once") {} }
  when (fire(18)) { a.where(_.once =/= 5.U).only.event("not_five") {} }
  when (fire(19)) { a.where(_.once === 0.U).head.event("zero_once") {} }
  when (fire(20)) { EventTrace.event("census") { "first" := a.head } }
  when (fire(21)) { EventTrace.create(new Note, event = "jot") { n => n.v := 1.U; "self" := n } }
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
