package eventtrace

import chisel3.experimental.BaseModule

/** A field's type as consumers see it; `enumDef` is set for a `ChiselEnum` field, and `idOf` names the object type
  * of a captured object ID.
  */
private[eventtrace] case class FieldType(
  width:   Int,
  signed:  Boolean,
  isBool:  Boolean,
  enumDef: Option[EnumDef],
  idOf:    Option[String] = None) {
  def kind: String =
    if (idOf.isDefined) "Id" else if (enumDef.isDefined) "Enum" else if (isBool) "Bool" else if (signed) "SInt" else "UInt"

  /** What sites sharing an event name must agree on: an enum field's enum, not merely its width. */
  def typeName: String =
    idOf.map(t => s"Id<$t>").orElse(enumDef.map(_.name)).getOrElse(if (isBool) "Bool" else s"$kind<$width>")
}

private[eventtrace] object FieldType {

  /** The ID of an object of type `objectType`, which the runtime counts in 64 bits. */
  def id(objectType: String): FieldType = FieldType(64, signed = false, isBool = false, None, Some(objectType))
}

/** Where a field's value comes from when a record is decoded. */
private[eventtrace] sealed trait FieldSource

/** Carried in the payload, starting at this bit. */
private[eventtrace] case class Packed(offset: Int) extends FieldSource

/** Captured from a literal, so never transmitted; the decoder supplies it. */
private[eventtrace] case class Constant(value: BigInt) extends FieldSource

/** Left `Absent` at this site, so left out of its records. */
private[eventtrace] case object Omitted extends FieldSource

/** The ID of the object `handle` resolves to, which the runtime supplies. */
private[eventtrace] case class Reference(handle: ObjectType) extends FieldSource

/** One field of a site. An event-local field left `Absent` has no type until [[EventSchema.resolve]] takes it from
  * a site sharing its event name.
  */
private[eventtrace] case class FieldLayout(name: String, tpe: Option[FieldType], source: FieldSource) {
  private def resolved: FieldType = tpe.getOrElse(throw new IllegalStateException(s"field $name has no type yet"))
  def width: Int = resolved.width
  def signed: Boolean = resolved.signed
  def enumDef: Option[EnumDef] = resolved.enumDef
  def typeName: String = tpe.map(_.typeName).getOrElse("Absent")
}

/** One emission site, as recorded at elaboration, in the module instance that declared it. Its sink carries
  * `payloadWords` 32-bit words.
  *
  * @param written   the event name as written at the site, such as `arrive`
  * @param subject   the subject's object type, such as `packet`, for an event with a subject
  * @param lifecycle `create` for a create event, `destroy` for one that destroys its subject
  * @param plan      how the runtime handles the site's records, for an event with a subject, a captured ID, or a test
  */
private[eventtrace] case class SiteLayout(
  id:           Int,
  written:      String,
  subject:      Option[String],
  lifecycle:    Option[String],
  location:     String,
  fields:       Seq[FieldLayout],
  payloadWords: Int,
  module:       BaseModule,
  plan:         Option[SitePlan]) {

  /** The event's name as consumers see it: `packet.arrive` for an event with a subject, else as written. */
  def name: String = SiteLayout.qualifiedName(subject, written)

  /** The declaring module's Chisel instance path, such as `ChipTop.tile.dcache`; valid once elaboration has finished,
    * when every parent has named its children.
    */
  def instance: String = module.pathName
}

/** What a site's handler does besides decoding its fields: test the views that gate it, resolve handles, then apply
  * effects to the subject.
  *
  * @param subject  the subject, if any: a create block's new object, a chosen object, or each member of a view
  * @param handles  every handle the site resolves, the subject among them, in construction order, so that each
  *                 comes after the handles its query refers to
  * @param operands the hardware values its queries compare with, and the bit where each starts in the payload
  * @param assigned the subject's fields this site assigns a value, which are also record fields
  * @param effects  membership changes and destruction, in the order written
  * @param guards   the tests the site's records must pass, outermost first
  */
private[eventtrace] case class SitePlan(
  subject:  Option[HandlePlan],
  handles:  Seq[HandlePlan],
  operands: Seq[(Operand.Hardware, Int)],
  assigned: Seq[ObjectField],
  effects:  Seq[Effect],
  guards:   Seq[Guard])

/** One handle a site resolves, as bound when the site was elaborated. */
private[eventtrace] case class HandlePlan(handle: ObjectType, binding: Binding) {
  def typeName: String = handle.typeName
}

private[eventtrace] object SiteLayout {
  def qualifiedName(subject: Option[String], written: String): String = subject.fold(written)(s => s"$s.$written")
}

/** The format consumers see for one event name, whichever site emitted it: its fields, sorted by name, and which of
  * them some site leaves `Absent`.
  */
private[eventtrace] case class EventSchema(
  name:      String,
  written:   String,
  subject:   Option[String],
  lifecycle: Option[String],
  fields:    Seq[FieldLayout],
  optional:  Set[String],
  sites:     Seq[SiteLayout])

private[eventtrace] object EventSchema {

  /** Gives each event-local field left `Absent` the type its namesake sites declare. Sites sharing a name were
    * checked at elaboration to agree on every type they declare, but a site that elaborated earlier may lack one.
    *
    * @return the sites with every field typed, or the fields that no site gives a type, with their sites' locations
    */
  def resolve(sites: Seq[SiteLayout]): Either[Seq[String], Seq[SiteLayout]] = {
    val types = sites.groupBy(_.name).map { case (name, group) =>
      name -> group.flatMap(_.fields).collect { case FieldLayout(field, Some(t), _) => field -> t }.toMap
    }
    val untyped = sites.groupBy(_.name).toSeq.sortBy(_._1).flatMap { case (name, group) =>
      group.flatMap(_.fields.map(_.name)).distinct.filterNot(types(name).contains).map { field =>
        s"""field "$field" of event "$name" is Absent at every site (${group.map(_.location).mkString(", ")}), so it has no type"""
      }
    }
    if (untyped.nonEmpty) Left(untyped)
    else Right(sites.map(s => s.copy(fields = s.fields.map(f => f.copy(tpe = Some(types(s.name)(f.name)))))))
  }

  /** Builds each event's schema from sites whose fields all have types. */
  def fromSites(sites: Seq[SiteLayout]): Seq[EventSchema] =
    sites.groupBy(_.name).toSeq.sortBy(_._1).map { case (name, group) =>
      val first = group.minBy(_.id)
      val optional = group.flatMap(_.fields).collect { case FieldLayout(field, _, Omitted) => field }.toSet
      EventSchema(name, first.written, first.subject, first.lifecycle, first.fields.sortBy(_.name), optional, group.sortBy(_.id))
    }
}
