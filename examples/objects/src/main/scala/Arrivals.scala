package eventtrace.examples.objects

import chisel3._
import circt.stage.ChiselStage
import eventtrace.EventTrace
import eventtrace.EventTrace.syntax._

import java.nio.file.{Files, Paths}

class Packet extends EventTrace.ObjectType("packet") {
  val seq  = const(UInt(8.W))
  val kind = setOnce(UInt(2.W))
}
object Packet extends EventTrace.Schema(new Packet)

class Token extends EventTrace.ObjectType("token") {
  val slot = const(UInt(4.W))
}
object Token extends EventTrace.Schema(new Token)

/** Objects fixture: two create sites share the event packet.arrive, and a third creates tokens. Every 24th cycle,
  * starting at 0, all three fire, so packets and tokens are numbered in site order within the cycle.
  */
class Arrivals extends Module {
  val io = IO(new Bundle {
    val count = Output(UInt(8.W))
  })

  val count = RegInit(0.U(8.W))
  count := count + 1.U
  io.count := count

  when (count(1, 0) === 0.U) {   // site 0: every 4th cycle
    Packet.create(event = "arrive") { p => p.seq := count; p.kind := count(3, 2); "lane" := 0.U(1.W) }
  }
  when (count(2, 0) === 0.U) {   // site 1: every 8th cycle; it has no kind, so its records leave kind out
    Packet.create(event = "arrive") { p => p.seq := count; p.kind := Absent; "lane" := 1.U(1.W) }
  }
  when (count % 3.U === 0.U) {   // site 2: every 3rd cycle
    Token.create(event = "grant") { t => t.slot := count(3, 0) }
  }
}

object Main extends App {
  EventTrace.enable()
  val targetDir = args.headOption.getOrElse("build/rtl")
  ChiselStage.emitSystemVerilogFile(
    new Arrivals,
    args = Array("--target-dir", targetDir, "--split-verilog"),
    firtoolOpts = Array("-disable-all-randomization", "-strip-debug-info")
  )
  // The manifest describes the design for tools outside the simulator, so the flow writes it after elaborating.
  Files.writeString(Paths.get(targetDir, "eventtrace_manifest.json"), EventTrace.manifest)
}
