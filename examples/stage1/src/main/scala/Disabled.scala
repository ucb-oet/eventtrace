package eventtrace.examples.stage1

import circt.stage.ChiselStage
import eventtrace.EventTrace

/** Passes only if, with tracing left off, the stage1 fixture elaborates no sinks, carries no generated files, and
  * declares no sites.
  */
object Disabled extends App {
  val chirrtl = ChiselStage.emitCHIRRTL(new Conditional)
  val leftovers = Seq("EventTraceSink", "eventtrace_sites.h", "eventtrace_runtime").filter(chirrtl.contains)
  if (leftovers.nonEmpty) throw new RuntimeException(s"FAIL: with tracing off, the CHIRRTL still has ${leftovers.mkString(", ")}")
  if (EventTrace.manifest.contains("site_id")) throw new RuntimeException("FAIL: with tracing off, the manifest lists sites")
  println("PASS: with tracing off, the fixture elaborates no sinks, carries no generated files, and declares no sites")
}
