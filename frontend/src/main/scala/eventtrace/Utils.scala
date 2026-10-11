package eventtrace

import chisel3._
import chisel3.experimental.SourceInfo
import chisel3.internal.eventtrace.WhenDepth

/** Small hardware helpers for instrumentation.
  *
  * `rising` and `falling` turn a condition that holds for several cycles into one event at its start or end:
  * {{{
  * val stallBegins = Utils.rising(stall)
  * when (stallBegins) {
  *   waiting.event("stall") { "blocker" := blocker }
  * }
  * }}}
  * Each marks one run of the signal, so use them where one run is one thing worth reporting. Each creates a register,
  * so compute it into a `val` outside any `when`, as any condition that builds state around an event must be: an
  * event's enable evaluates the conditions around it again, which would build a second register inside the `when`.
  */
object Utils {

  /** High on the first cycle of each run of `c`: `c` is high and was low on the previous cycle. */
  def rising(c: Bool)(implicit sourceInfo: SourceInfo): Bool = edge("rising", c && !RegNext(c, false.B))

  /** High on the first cycle after each run of `c`: `c` is low and was high on the previous cycle. */
  def falling(c: Bool)(implicit sourceInfo: SourceInfo): Bool = edge("falling", !c && RegNext(c, false.B))

  private def edge(name: String, build: => Bool)(implicit sourceInfo: SourceInfo): Bool = {
    if (EventTrace.computingEnable.value)
      EventTrace.elaborationError(
        s"Utils.$name creates a register, and an event inside a `when` evaluates the `when`'s condition again, " +
          "which would build a second register; compute the edge into a `val` before the `when`",
        sourceInfo
      )
    if (Module.currentModule.isEmpty) EventTrace.elaborationError(s"Utils.$name is outside a module", sourceInfo)
    if (WhenDepth() > 0)
      EventTrace.elaborationError(
        s"Utils.$name creates a register, so call it outside any `when`: inside one, the register would update " +
          "only while the enclosing condition holds",
        sourceInfo
      )
    build
  }
}
