package eventtrace.examples.stage1

import chisel3._
import chisel3.util.Cat
import circt.stage.ChiselStage
import eventtrace.{EventTrace, Utils}
import eventtrace.EventTrace.capture
import eventtrace.EventTrace.syntax._

import java.nio.file.{Files, Paths}

/** Three variants in two bits, so the fourth bit pattern is not a variant. */
object Mode extends ChiselEnum {
  val Off, Slow, Fast = Value
}

/** Instantiated twice, so its one event becomes two sites that only their instance paths tell apart. */
class Lane extends Module {
  val io = IO(new Bundle {
    val in = Input(UInt(8.W))
  })
  when (io.in(0)) {
    EventTrace.event("lane") { "value" := io.in }
  }
}

/** Stage 1 fixture: sites under when/elsewhen/otherwise inside an outer when, one unconditional site, one event in a
  * module instantiated twice, one event with enum fields, one event whose payload spans three words, and one event
  * whose field is DontCare at one of its two sites, and events at the edges of counter bits.
  */
class Conditional extends Module {
  val io = IO(new Bundle {
    val count = Output(UInt(8.W))
  })

  val count = RegInit(0.U(8.W))
  count := count + 1.U
  io.count := count

  // Outside any `when`: fires every cycle and carries no fields.
  EventTrace.event("tick") {}

  val phase = count(1, 0)
  when (count(2)) {
    when (phase === 0.U) {
      EventTrace.event("first") {
        "count" := count
      }
    }.elsewhen (phase === 1.U) {
      EventTrace.event("second") {
        capture("count", count)
        "sum" := count + 3.U
      }
    }.elsewhen (phase === 2.U) {
      EventTrace.event("rest") {
        "phase" := phase
        "odd" := count(0)
        "path" := 0.U(1.W)
      }
    }.otherwise {
      // The same event as the previous site: the same fields, declared in a different order.
      // `path` tells the two sites apart; it is a literal at both, so it is never transmitted.
      EventTrace.event("rest") {
        capture("path", 1.U(1.W))
        capture("odd", count(0))
        capture("phase", phase)
      }
    }
  }

  // An enum read from hardware, whose value 3 is not a variant, and an enum literal, which is never transmitted.
  val mode = Mode.safe(count(1, 0))._1
  when (count(3)) {
    EventTrace.event("mode") {
      "mode" := mode
      "target" := Mode.Fast
    }
  }

  // 76 payload bits in three 32-bit words; `b` straddles words 1 and 2.
  when (count(4)) {
    EventTrace.event("wide") {
      "a" := Cat(count, count, count, count)
      "b" := Cat(count(3, 0), count, count, count, count)
      "c" := count
    }
  }

  // One event at two sites. The first, elaborated earlier, leaves `cause` DontCare, so it takes its type from the
  // second when the generated files are written, and its records leave `cause` out.
  when (count === 5.U) {
    EventTrace.event("flush") { "cause" := DontCare }
  }
  when (count === 6.U) {
    EventTrace.event("flush") { "cause" := count(1, 0) }
  }

  // Edge helpers, each computed into a val before its `when`.
  for (b <- Seq(1, 2)) {
    val rise = Utils.rising(count(b))
    when (rise) {
      EventTrace.event(s"rise$b") {}
    }
  }
  val fall2 = Utils.falling(count(2))
  when (fall2) {
    EventTrace.event("fall2") {}
  }

  // lane0 sees odd values on odd cycles and lane1 on even cycles, so exactly one lane fires each cycle.
  val lane0 = Module(new Lane)
  lane0.io.in := count
  val lane1 = Module(new Lane)
  lane1.io.in := count + 1.U
}

object Main extends App {
  EventTrace.enable()
  val targetDir = args.headOption.getOrElse("build/rtl")
  ChiselStage.emitSystemVerilogFile(
    new Conditional,
    args = Array("--target-dir", targetDir, "--split-verilog"),
    firtoolOpts = Array("-disable-all-randomization", "-strip-debug-info")
  )
  // The manifest describes the design for tools outside the simulator, so the flow writes it after elaborating.
  Files.writeString(Paths.get(targetDir, "eventtrace_manifest.json"), EventTrace.manifest)
}
