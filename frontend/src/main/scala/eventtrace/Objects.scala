package eventtrace

import chisel3._
import chisel3.experimental.SourceInfo

/** How often an object field may be assigned. */
sealed trait FieldProperty
object FieldProperty {

  /** Initialized when the object is created and never assigned again. */
  case object Const extends FieldProperty

  /** Initially unset; assigned at most once. */
  case object SetOnce extends FieldProperty

  /** Initially unset; assigned any number of times. */
  case object Mutable extends FieldProperty
}

/** A tracked object's schema. Each instance is a fresh handle, as with Chisel Bundles:
  *
  * {{{
  * class Packet extends EventTrace.ObjectType("packet") {
  *   val seq  = const(UInt(8.W))
  *   val kind = setOnce(UInt(2.W))
  * }
  * object Packet extends EventTrace.Schema(new Packet)
  * }}}
  *
  * Field names come from the `val` names. All instances of one object type must declare the same fields.
  */
abstract class ObjectType(val typeName: String) {
  private val declared = scala.collection.mutable.ArrayBuffer.empty[ObjectField]

  /** Whether this handle's create block is running, has closed, or has not started. */
  private[eventtrace] var scope: ObjectType.Scope = ObjectType.Unbound

  private[eventtrace] def fields: Seq[ObjectField] = declared.toSeq

  protected def const[T <: Bits](gen: T)(implicit name: sourcecode.Name, sourceInfo: SourceInfo): BitsField =
    declare(new BitsField(this, name.value, EventTrace.typeOf(gen, None, name.value, sourceInfo), FieldProperty.Const))
  protected def setOnce[T <: Bits](gen: T)(implicit name: sourcecode.Name, sourceInfo: SourceInfo): BitsField =
    declare(new BitsField(this, name.value, EventTrace.typeOf(gen, None, name.value, sourceInfo), FieldProperty.SetOnce))
  protected def mutable[T <: Bits](gen: T)(implicit name: sourcecode.Name, sourceInfo: SourceInfo): BitsField =
    declare(new BitsField(this, name.value, EventTrace.typeOf(gen, None, name.value, sourceInfo), FieldProperty.Mutable))

  protected def const[T <: EnumType](gen: T)(implicit ev: EnumOf[T], name: sourcecode.Name, sourceInfo: SourceInfo): EnumField[T] =
    declare(new EnumField[T](this, name.value, EventTrace.typeOf(gen, Some(ev.enumObject), name.value, sourceInfo), FieldProperty.Const))
  protected def setOnce[T <: EnumType](gen: T)(implicit ev: EnumOf[T], name: sourcecode.Name, sourceInfo: SourceInfo): EnumField[T] =
    declare(new EnumField[T](this, name.value, EventTrace.typeOf(gen, Some(ev.enumObject), name.value, sourceInfo), FieldProperty.SetOnce))
  protected def mutable[T <: EnumType](gen: T)(implicit ev: EnumOf[T], name: sourcecode.Name, sourceInfo: SourceInfo): EnumField[T] =
    declare(new EnumField[T](this, name.value, EventTrace.typeOf(gen, Some(ev.enumObject), name.value, sourceInfo), FieldProperty.Mutable))

  private def declare[F <: ObjectField](field: F): F = {
    declared += field
    field
  }
}

object ObjectType {
  private[eventtrace] sealed trait Scope
  private[eventtrace] case object Unbound extends Scope
  private[eventtrace] case object Open extends Scope
  private[eventtrace] case object Closed extends Scope
}

/** One field of an object handle. Assigning it inside the handle's create block records the value in the create
  * event's record; `DontCare` declares the field without a value.
  */
sealed abstract class ObjectField(
  private[eventtrace] val owner:    ObjectType,
  val name:                         String,
  private[eventtrace] val tpe:      FieldType,
  private[eventtrace] val property: FieldProperty) {

  def :=(value: DontCare.type)(implicit sourceInfo: SourceInfo): Unit = EventTrace.assignField(this, None, sourceInfo)
}

/** A `Bool`, `UInt`, or `SInt` field. A narrower value is extended; a wider one, or a mix of `UInt` and `SInt`, is an
  * elaboration error. `Bool` and `UInt<1>` convert both ways.
  */
final class BitsField private[eventtrace] (owner: ObjectType, name: String, tpe: FieldType, property: FieldProperty)
    extends ObjectField(owner, name, tpe, property) {
  def :=(value: Bits)(implicit sourceInfo: SourceInfo): Unit = EventTrace.assignField(this, Some(value), sourceInfo)
}

/** A `ChiselEnum` field; its static type admits only values of the same enum. */
final class EnumField[T <: EnumType] private[eventtrace] (owner: ObjectType, name: String, tpe: FieldType, property: FieldProperty)
    extends ObjectField(owner, name, tpe, property) {
  def :=(value: T)(implicit sourceInfo: SourceInfo): Unit = EventTrace.assignField(this, Some(value), sourceInfo)
}

/** The companion of an object type, which creates objects: `Packet.create(event = "arrive") { p => ... }`. */
abstract class Schema[T <: ObjectType](gen: => T) {

  /** Creates an object of this type on every clock edge at which the enclosing `when` conditions hold. The block
    * receives the new object's handle, which exists only inside it, and initializes its fields. The create event is
    * named after the object type and `event`, such as `packet.arrive`.
    */
  def create(event: String)(body: T => Unit)(implicit sourceInfo: SourceInfo): Unit = {
    val handle = gen
    EventTrace.createSite(handle, event, sourceInfo)(body(handle))
  }
}
