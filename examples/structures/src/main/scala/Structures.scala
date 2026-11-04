/* Structures fixture: a small out-of-order pipeline whose instructions are tracked through a queue, a ROB, and a
 * map, the three kinds of collection besides a set. */
package eventtrace.examples.structures

import chisel3._
import chisel3.util.PriorityEncoder
import circt.stage.ChiselStage
import eventtrace.EventTrace
import eventtrace.EventTrace.syntax._

import java.nio.file.{Files, Paths}

/** An instruction, from fetch until it commits or is squashed. */
class Insn extends EventTrace.ObjectType("insn") {
  val pc = const(UInt(8.W))
}

/** An instruction's operation while it waits in the reservation station. */
class Uop extends EventTrace.ObjectType("uop") {
  val pc   = const(UInt(8.W))
  val insn = parent(new Insn)
}

/** A pipeline with a four-entry fetch queue, an eight-entry ROB tagged {wrap, index}, and a four-slot reservation
  * station. Each cycle the front end fetches the next PC; dispatch moves the fetch queue's head into the ROB's tail
  * and the lowest free slot, which waits ((PC mod 8) xor 5) or (PC bit 3, shifted to bit 2) cycles; the lowest slot
  * whose wait has run out executes, so operations finish out of order; and the ROB's head commits once done. A PC of
  * 5 mod 8 is a branch that mispredicts when it executes, at once for 5 mod 16 and after 4 cycles for 13 mod 16: the
  * ROB's tail moves back to just after it, younger operations leave the reservation station, the fetch queue
  * empties, and fetch restarts after the branch. A PC of 14 mod 16 is a jump that dispatch resolves: the fetch
  * queue's younger entries leave, and fetch restarts at the jump's PC + 3.
  */
class Structures extends Module {
  val io = IO(new Bundle {
    val pc = Output(UInt(8.W))
  })

  val nextPc  = RegInit(0.U(8.W))
  val fqPc    = Reg(Vec(4, UInt(8.W)))
  val fqHead  = RegInit(0.U(2.W))
  val fqCount = RegInit(0.U(3.W))

  val robValid = RegInit(VecInit(Seq.fill(8)(false.B)))
  val robDone  = Reg(Vec(8, Bool()))
  val robPc    = Reg(Vec(8, UInt(8.W)))
  val robHead  = RegInit(0.U(4.W))
  val robTail  = RegInit(0.U(4.W))
  val robCount = robTail - robHead

  val rsValid = RegInit(VecInit(Seq.fill(4)(false.B)))
  val rsPc    = Reg(Vec(4, UInt(8.W)))
  val rsTag   = Reg(Vec(4, UInt(4.W)))
  val rsWait  = Reg(Vec(4, UInt(3.W)))
  io.pc := nextPc

  // Execute: the lowest slot whose wait has run out.
  val ready      = VecInit((0 until 4).map(s => rsValid(s) && rsWait(s) === 0.U)).asUInt
  val exe        = ready.orR
  val exeSlot    = PriorityEncoder(ready)
  val exePc      = rsPc(exeSlot)
  val exeTag     = rsTag(exeSlot)
  val mispredict = exe && exePc(2, 0) === 5.U
  val cut        = exeTag + 1.U

  // Commit: the ROB's head, once done.
  val headIdx = robHead(2, 0)
  val commit  = robValid(headIdx) && robDone(headIdx)

  // Dispatch: the fetch queue's head into the ROB's tail and the lowest free slot.
  val free     = VecInit(rsValid.map(!_)).asUInt
  val dispSlot = PriorityEncoder(free)
  val dispPc   = fqPc(fqHead)
  val dispatch = fqCount =/= 0.U && robCount =/= 8.U && free.orR && !mispredict
  val jump     = dispatch && dispPc(3, 0) === 14.U

  // Fetch: the next PC into the fetch queue's tail.
  val fetch = fqCount =/= 4.U && !mispredict && !jump

  for (s <- 0 until 4) when (rsValid(s) && rsWait(s) =/= 0.U) { rsWait(s) := rsWait(s) - 1.U }
  when (exe) {
    rsValid(exeSlot) := false.B
    robDone(exeTag(2, 0)) := true.B
  }
  when (commit) {
    robValid(headIdx) := false.B
    robHead := robHead + 1.U
  }
  when (mispredict) {
    for (e <- 0 until 8) {
      val younger = (e.U(3.W) - cut(2, 0)) < (robTail - cut)
      when (younger) { robValid(e) := false.B }
    }
    robTail := cut
    for (s <- 0 until 4) when (rsPc(s) > exePc) { rsValid(s) := false.B }
    fqCount := 0.U
    nextPc := exePc + 1.U
  }.otherwise {
    when (dispatch) {
      robValid(robTail(2, 0)) := true.B
      robDone(robTail(2, 0)) := false.B
      robPc(robTail(2, 0)) := dispPc
      robTail := robTail + 1.U
      rsValid(dispSlot) := true.B
      rsPc(dispSlot) := dispPc
      rsTag(dispSlot) := robTail
      rsWait(dispSlot) := (dispPc(2, 0) ^ 5.U) | (dispPc(3) << 2)
      fqHead := fqHead + 1.U
    }
    when (fetch) {
      fqPc(fqHead + fqCount(1, 0)) := nextPc
      nextPc := nextPc + 1.U
    }
    fqCount := fqCount + fetch.asUInt - dispatch.asUInt
    when (jump) {
      fqCount := 0.U
      nextPc := dispPc + 3.U
    }
  }

  // ---- Trace

  val fetchQueue = EventTrace.queue(new Insn, entries = 4)
  val rob        = EventTrace.rob(new Insn, entries = 8, keys = 16)
  val station    = EventTrace.map(new Uop, entries = 4)

  when (fetch) {
    EventTrace.create(new Insn, event = "fetch") { i => i.pc := nextPc; fetchQueue.add(i) }
  }
  when (dispatch) {
    val i = fetchQueue.pop()
    i.event("dispatch") { rob.add(robTail, i) }
    EventTrace.create(new Uop, event = "dispatch") { u => u.pc := dispPc; u.insn := i; station.add(dispSlot, u) }
  }
  when (jump) {
    fetchQueue.dropYoungest(fqCount - 1.U) { i => i.event("flush") { i.abort() } }
  }
  when (exe) {
    val u = station(exeSlot)
    u.event("execute") { station.remove(u); u.complete() }
    rob(exeTag).event("complete") { "mispredict" := mispredict }
  }
  when (mispredict) {
    rob.rollback(cut) { i => i.event("squash") { i.abort() } }
    station.where(_.pc > exePc).iter { u => u.event("squash") { station.remove(u); u.abort() } }
    fetchQueue.clear { i => i.event("flush") { i.abort() } }
  }
  when (commit) {
    val h = rob.pop()
    h.event("commit") { h.complete() }
  }
}

object Main extends App {
  EventTrace.enable()
  val targetDir = args.headOption.getOrElse("build/rtl")
  ChiselStage.emitSystemVerilogFile(
    new Structures,
    args = Array("--target-dir", targetDir, "--split-verilog"),
    firtoolOpts = Array("-disable-all-randomization", "-strip-debug-info")
  )
  // The manifest describes the design for tools outside the simulator, so the flow writes it after elaborating.
  Files.writeString(Paths.get(targetDir, "eventtrace_manifest.json"), EventTrace.manifest)
}
