/* Enums in traces: the evidence that ties a ChiselEnum value to its enum object, and an enum as the manifest and the
 * generated C++ describe it. */
package eventtrace

import chisel3.ChiselEnum

import scala.annotation.implicitNotFound

/** Evidence of the `ChiselEnum` object that a value's static type belongs to.
  *
  * All enums share one runtime `Type` class, and Chisel keeps the link from a value to its enum private. A value's
  * static type, such as `Stage.Type`, does name its enum, so the compiler supplies this evidence from the type:
  * users write `"stage" := stageReg` and never mention it. A helper generic over enums must pass it along with
  * `(implicit ev: EnumOf[T])`; a value widened to plain `EnumType` has lost its enum and does not compile.
  */
@implicitNotFound("${T} is not the Type of a ChiselEnum object, so EventTrace cannot find its variant names")
trait EnumOf[T] {

  /** The `ChiselEnum` object whose `Type` is `T`. */
  def enumObject: ChiselEnum
}

/** Where the compiler finds [[EnumOf]] evidence for a value's static type. */
object EnumOf {

  /** Evidence for `E#Type`, an enum's `Type`: `ValueOf` supplies `E`'s only value, the enum object itself. */
  implicit def fromType[E <: ChiselEnum with Singleton](implicit e: ValueOf[E]): EnumOf[E#Type] =
    new EnumOf[E#Type] { def enumObject: ChiselEnum = e.value }
}

/** One enum as consumers see it: its simple name and its variants' names and values, in declaration order. */
private[eventtrace] case class EnumDef(name: String, fullName: String, width: Int, variants: Seq[(String, BigInt)])

/** Builds an [[EnumDef]] from a `ChiselEnum` object. */
private[eventtrace] object EnumDef {

  /** `Stage` for `eventtrace.examples.Stage`, and `Inner` for an object nested as `Outer.Inner`. */
  def simpleName(e: ChiselEnum): String = fullName(e).split("[.$]").last

  /** The Scala object's fully qualified class name, without the trailing `$`. */
  def fullName(e: ChiselEnum): String = e.getClass.getName.stripSuffix("$")

  /** `e` as consumers see it: its simple and full names, its width, and its variants with their values. */
  def apply(e: ChiselEnum): EnumDef =
    EnumDef(simpleName(e), fullName(e), e.getWidth, e.allNames.zip(e.all.map(_.litValue)))
}
