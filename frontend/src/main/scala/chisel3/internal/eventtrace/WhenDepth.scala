package chisel3.internal.eventtrace

/** EventTrace's uses of Chisel internals, which code inside the `chisel3` package may read from Chisel's private
  * `Builder`. If a Chisel release moves either, only this file changes.
  */

/** Chisel tracks the enclosing `when` scopes only in its `Builder`. EventTrace compares this depth when an event opens
  * and at each capture, to reject captures under a `when` nested inside the event.
  */
object WhenDepth {
  def apply(): Int = chisel3.internal.Builder.whenDepth
}

/** The current elaboration, as a token to compare by identity. EventTrace starts its records afresh in each
  * elaboration, since one program may elaborate several designs.
  */
object Elaboration {
  def current(): AnyRef = chisel3.internal.Builder.captureContext()
}
