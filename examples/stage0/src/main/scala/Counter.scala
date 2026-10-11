package eventtrace.examples.stage0

import chisel3._
import circt.stage.ChiselStage
import eventtrace.EventTrace
import eventtrace.EventTrace.syntax._

import java.nio.file.{Files, Paths}

/** Stage 0b fixture: one two-field event every eighth cycle. */
class Counter extends Module {
  val io = IO(new Bundle {
    val count = Output(UInt(32.W))
  })

  // Start near the top of the range so `count` also exercises the payload's upper word.
  val count = RegInit("hffffffe0".U(32.W))
  count := count + 1.U
  io.count := count

  when (count(2, 0) === 0.U) {
    EventTrace.event("tick") {
      // `delta` is signed and comes first, so `count` (bits 39..8) straddles a 32-bit word boundary.
      "delta" := count(7, 0).asSInt - 20.S
      "count" := count
    }
  }
}

object Main extends App {
  EventTrace.enable()
  val targetDir = args.headOption.getOrElse("build/rtl")
  ChiselStage.emitSystemVerilogFile(
    new Counter,
    args = Array("--target-dir", targetDir, "--split-verilog"),
    firtoolOpts = Array("-disable-all-randomization", "-strip-debug-info")
  )
  // The manifest describes the design for tools outside the simulator, so the flow writes it after elaborating.
  Files.writeString(Paths.get(targetDir, "eventtrace_manifest.json"), EventTrace.manifest)
}
