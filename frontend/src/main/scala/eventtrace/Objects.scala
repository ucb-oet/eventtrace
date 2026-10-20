package eventtrace

import chisel3._
import chisel3.experimental.SourceInfo

/** What a field that names another object means, beyond its value: the object one came from, the object that
  * aborted it, or the object it is blocked by. Viewers draw these links differently from other fields.
  */
sealed abstract class FieldRole(val written: String)
object FieldRole {
  case object Parent extends FieldRole("parent")
  case object AbortedBy extends FieldRole("aborted_by")
  case object BlockedBy extends FieldRole("blocked_by")
}

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
  * EventTrace.create(new Packet, event = "arrive") { p => p.seq := count }
  * }}}
  *
  * Field names come from the `val` names. All instances of one object type must declare the same fields. A handle
  * comes from a create block, valid only inside it, or from a query over a collection.
  */
abstract class ObjectType(val typeName: String) {
  private val declared = scala.collection.mutable.ArrayBuffer.empty[ObjectField]

  /** What produced this handle, which decides where it may be used. */
  private[eventtrace] var binding: Binding = Binding.Unbound

  /** Handles are numbered in construction order, so a handle is always numbered after those its query refers to. */
  private[eventtrace] val serial: Long = ObjectType.nextSerial()

  private[eventtrace] def fields: Seq[ObjectField] = declared.toSeq

  /** Emits a record whose subject is this object on every clock edge at which the enclosing `when` conditions hold.
    * The event is named after the object type, such as `instruction.issue`. The block's effects act on this object.
    */
  final def event(name: String)(body: => Unit)(implicit sourceInfo: SourceInfo): Unit =
    EventTrace.objectEvent(this, name, sourceInfo)(body)

  /** Ends this object's life normally, having done its work, when the open event fires, as a retire does. Only the
    * event's subject may end, and it must have left every collection by then.
    */
  final def complete()(implicit sourceInfo: SourceInfo): Unit = EventTrace.end(this, normal = true, None, sourceInfo)

  /** Ends this object's life abnormally, before it finished its work, when the open event fires, as a squash does. */
  final def abort()(implicit sourceInfo: SourceInfo): Unit = EventTrace.end(this, normal = false, None, sourceInfo)

  /** Ends this object's life abnormally because of `by`, which the record names in its field `by`. `Absent` declares
    * the field at a site that has no object to name, as its namesakes do.
    */
  final def abort(by: ObjectType)(implicit sourceInfo: SourceInfo): Unit =
    EventTrace.end(this, normal = false, Some(Some(by)), sourceInfo)
  final def abort(by: IdRef)(implicit sourceInfo: SourceInfo): Unit =
    EventTrace.end(this, normal = false, Some(Some(by.handle)), sourceInfo)
  final def abort(by: EventTrace.syntax.Absent.type)(implicit sourceInfo: SourceInfo): Unit =
    EventTrace.end(this, normal = false, Some(None), sourceInfo)

  /** Says that this event finds this object blocked: waiting, rather than making progress. */
  final def blocked()(implicit sourceInfo: SourceInfo): Unit = EventTrace.blocked(this, None, sourceInfo)

  /** Says that this event finds this object blocked by `by`, which the record names in its field `by`. `Absent`
    * declares the field at a site that has no object to name, as its namesakes do.
    */
  final def blocked(by: ObjectType)(implicit sourceInfo: SourceInfo): Unit =
    EventTrace.blocked(this, Some(Some(by)), sourceInfo)
  final def blocked(by: IdRef)(implicit sourceInfo: SourceInfo): Unit =
    EventTrace.blocked(this, Some(Some(by.handle)), sourceInfo)
  final def blocked(by: EventTrace.syntax.Absent.type)(implicit sourceInfo: SourceInfo): Unit =
    EventTrace.blocked(this, Some(None), sourceInfo)

  /** This object's ID: compare it in a query, or capture it as an event field, as in `"blocker" := blocker.id`. */
  final def id: IdRef = new IdRef(this)

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

  /** A field holding the ID of another object, which the create block sets, as in `val fetch = const(Id(new Fetch))`.
    * Only a `const` field may hold an ID: it names one object for this object's whole life.
    */
  protected def const[T <: ObjectType](gen: IdOf[T])(implicit name: sourcecode.Name, sourceInfo: SourceInfo): IdField[T] =
    declare(new IdField[T](this, name.value, EventTrace.idTypeOf(gen.typeName, name.value, sourceInfo), FieldProperty.Const))

  /** A `const` field holding the ID of the object this one came from, of `gen`'s type, such as an instruction's fetch
    * packet. A type has at most one.
    */
  protected def parent[T <: ObjectType](gen: T)(implicit name: sourcecode.Name, sourceInfo: SourceInfo): IdField[T] =
    declare(new IdField[T](this, name.value, EventTrace.idTypeOf(gen.typeName, name.value, sourceInfo), FieldProperty.Const,
      Some(FieldRole.Parent)))

  /** The type of a field holding the ID of an object of `gen`'s type, which an instance names, as for a collection. */
  protected def Id[T <: ObjectType](gen: T): IdOf[T] = new IdOf[T](gen.typeName)

  private def declare[F <: ObjectField](field: F): F = {
    declared += field
    field
  }
}

object ObjectType {
  private var serials = 0L
  private def nextSerial(): Long = { serials += 1; serials }
}

/** One field of an object handle. Assigning it inside an event on the handle records the value in the event's
  * record and, for an existing object, updates the object when the cycle ends; `Absent` declares the field without
  * a value. Inside `where`, comparing it builds a query.
  */
sealed abstract class ObjectField(
  private[eventtrace] val owner:    ObjectType,
  val name:                         String,
  private[eventtrace] val tpe:      FieldType,
  private[eventtrace] val property: FieldProperty,
  private[eventtrace] val role:     Option[FieldRole] = None)
    extends Key {

  private[eventtrace] def operand: Operand = Operand.Field(owner, this)

  def :=(value: EventTrace.syntax.Absent.type)(implicit sourceInfo: SourceInfo): Unit =
    EventTrace.assignField(this, None, sourceInfo)

  protected def compare(op: CompareOp, that: Data)(implicit sourceInfo: SourceInfo): Pred =
    Pred.Compare(op, operand, Queries.value(tpe, that, s"${owner.typeName}.$name ${op.written}", sourceInfo))

  protected def compare(op: CompareOp, that: ObjectField)(implicit sourceInfo: SourceInfo): Pred = {
    if (that.tpe.signed != tpe.signed)
      EventTrace.elaborationError(
        s"${owner.typeName}.$name ${op.written} compares a ${tpe.typeName} field with a ${that.tpe.typeName} field",
        sourceInfo
      )
    Pred.Compare(op, operand, that.operand)
  }
}

/** A `Bool`, `UInt`, or `SInt` field. A narrower value is extended; a wider one, or a mix of `UInt` and `SInt`, is an
  * elaboration error. `Bool` and `UInt<1>` convert both ways.
  */
final class BitsField private[eventtrace] (owner: ObjectType, name: String, tpe: FieldType, property: FieldProperty)
    extends ObjectField(owner, name, tpe, property) {
  def :=(value: Bits)(implicit sourceInfo: SourceInfo): Unit = EventTrace.assignField(this, Some(value), sourceInfo)

  def ===(that: Bits)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Eq, that)
  def =/=(that: Bits)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Ne, that)
  def <(that: Bits)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Lt, that)
  def <=(that: Bits)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Le, that)
  def >(that: Bits)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Gt, that)
  def >=(that: Bits)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Ge, that)

  def ===(that: BitsField)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Eq, that)
  def =/=(that: BitsField)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Ne, that)
  def <(that: BitsField)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Lt, that)
  def <=(that: BitsField)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Le, that)
  def >(that: BitsField)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Gt, that)
  def >=(that: BitsField)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Ge, that)
}

/** A `ChiselEnum` field; its static type admits only values of the same enum. Ordering compares variant values. */
final class EnumField[T <: EnumType] private[eventtrace] (owner: ObjectType, name: String, tpe: FieldType, property: FieldProperty)
    extends ObjectField(owner, name, tpe, property) {
  def :=(value: T)(implicit sourceInfo: SourceInfo): Unit = EventTrace.assignField(this, Some(value), sourceInfo)

  def ===(that: T)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Eq, that)
  def =/=(that: T)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Ne, that)
  def <(that: T)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Lt, that)
  def <=(that: T)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Le, that)
  def >(that: T)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Gt, that)
  def >=(that: T)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Ge, that)

  def ===(that: EnumField[T])(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Eq, that)
  def =/=(that: EnumField[T])(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Ne, that)
  def <(that: EnumField[T])(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Lt, that)
  def <=(that: EnumField[T])(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Le, that)
  def >(that: EnumField[T])(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Gt, that)
  def >=(that: EnumField[T])(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Ge, that)
}

/** The type of a field holding the ID of an object of type `T`; see [[ObjectType.Id]]. */
final class IdOf[T <: ObjectType] private[eventtrace] (private[eventtrace] val typeName: String)

/** A `const` field holding the ID of an object of type `T`. The create block assigns it a handle of that type, as in
  * `inst.fetch := packet`; queries cannot compare it yet.
  */
final class IdField[T <: ObjectType] private[eventtrace] (
  owner:    ObjectType,
  name:     String,
  tpe:      FieldType,
  property: FieldProperty,
  role:     Option[FieldRole] = None)
    extends ObjectField(owner, name, tpe, property, role) {
  def :=(handle: T)(implicit sourceInfo: SourceInfo): Unit = EventTrace.assignId(this, handle, sourceInfo)
  def :=(id: IdRef)(implicit sourceInfo: SourceInfo): Unit = EventTrace.assignId(this, id.handle, sourceInfo)
}
