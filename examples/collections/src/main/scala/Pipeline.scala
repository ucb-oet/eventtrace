package eventtrace.examples.collections

import chisel3._
import chisel3.util.PopCount
import circt.stage.ChiselStage
import eventtrace.{EventTrace, Utils}
import eventtrace.EventTrace.syntax._

import java.nio.file.{Files, Paths}

object Stage extends ChiselEnum {
  val Decode, Execute, Writeback = Value
}

class Instruction extends EventTrace.ObjectType("instruction") {
  val pc     = const(UInt(8.W))
  val odd    = const(Bool())
  val stage  = mutable(Stage())
  val result = setOnce(UInt(8.W))
}
object Instruction extends EventTrace.Schema(new Instruction)

/** Collections fixture: a three-stage in-order pipeline, decode, execute, and writeback, whose instructions are
  * tracked in one collection. The counter drives it: when it is 5 mod 11, the instruction in execute squashes the
  * one in decode; when it is 3 mod 7, decode waits a cycle on execute. Every other cycle, a new instruction with the
  * counter as its PC enters decode.
  */
class Pipeline extends Module {
  val io = IO(new Bundle {
    val count = Output(UInt(8.W))
  })

  val count = RegInit(0.U(8.W))
  count := count + 1.U
  io.count := count

  val decV  = RegInit(false.B)
  val decPc = Reg(UInt(8.W))
  val exeV  = RegInit(false.B)
  val exePc = Reg(UInt(8.W))
  val wbV   = RegInit(false.B)

  val kill  = exeV && count % 11.U === 5.U
  val stall = decV && exeV && !kill && count % 7.U === 3.U
  val issue = decV && !stall && !kill
  val enter = !stall && !kill

  wbV := exeV
  exeV := issue
  exePc := decPc
  decV := !kill
  when (enter) { decPc := count }

  val active = EventTrace.collection(new Instruction)

  // Site 0: join order is admission order.
  when (enter) {
    Instruction.create(event = "decode") { i =>
      i.pc := count; i.odd := count(0); i.stage := Stage.Decode
      active.add(i)
    }
  }
  // Site 1: found by its stage.
  when (issue) {
    val i = active.where(_.stage === Stage.Decode).only
    i.event("issue") { i.stage := Stage.Execute }
  }
  // Site 2: found by a hardware key, which travels in the payload.
  when (exeV) {
    val i = active.where(_.pc === exePc).only
    i.event("writeback") { i.stage := Stage.Writeback; i.result := exePc + 1.U }
  }
  // Site 3: the oldest instruction retires.
  when (wbV) {
    val i = active.head
    i.event("retire") { active.remove(i); i.destroy() }
  }
  // Site 4: one handle feeds another query; one record squashes every younger instruction, here at most one.
  when (kill) {
    val br = active.where(_.stage === Stage.Execute).only
    active.where(_.id > br.id).each { i =>
      i.event("squash") { "by" := br; active.remove(i); i.destroy() }
    }
  }
  // Site 5: an object reference as a field.
  val stallBegins = Utils.rising(stall)
  when (stallBegins) {
    val waiting = active.where(_.stage === Stage.Decode).only
    val blocker = active.where(_.stage === Stage.Execute).only
    waiting.event("stall") { "blocker" := blocker }
  }
  // Sites 6 and 7: two events per member, even PCs before odd ones and otherwise in join order.
  when (count % 5.U === 0.U) {
    active.orderBy(_.odd).each { i =>
      i.event("sample") { "pass" := 0.U(1.W) }
      i.event("sample") { "pass" := 1.U(1.W) }
    }
  }
  // Sites 8 and 9: a position in join order, and the head of a ranking by stage.
  when (count % 4.U === 2.U && PopCount(Seq(decV, exeV, wbV)) >= 2.U) {
    active.at(1).event("second") {}
    active.orderBy(_.stage).head.event("youngest") {}
  }
}

object Main extends App {
  EventTrace.enable()
  val targetDir = args.headOption.getOrElse("build/rtl")
  ChiselStage.emitSystemVerilogFile(
    new Pipeline,
    args = Array("--target-dir", targetDir, "--split-verilog"),
    firtoolOpts = Array("-disable-all-randomization", "-strip-debug-info")
  )
  // The manifest describes the design for tools outside the simulator, so the flow writes it after elaborating.
  Files.writeString(Paths.get(targetDir, "eventtrace_manifest.json"), EventTrace.manifest)
}
