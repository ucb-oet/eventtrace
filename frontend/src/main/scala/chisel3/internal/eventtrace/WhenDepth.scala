package chisel3.internal.eventtrace

/** EventTrace's only use of Chisel internals.
  *
  * Chisel tracks the enclosing `when` scopes only in its private `Builder`, which code inside the `chisel3` package
  * may read. EventTrace compares this depth when an event opens and at each capture, to reject captures under a `when`
  * nested inside the event. If a Chisel release moves the depth, only this file changes.
  */
object WhenDepth {
  def apply(): Int = chisel3.internal.Builder.whenDepth
}
