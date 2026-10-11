package eventtrace

import chisel3.experimental.BaseModule

/** A field's type as consumers see it; `enumDef` is set for a `ChiselEnum` field. */
private[eventtrace] case class FieldType(width: Int, signed: Boolean, isBool: Boolean, enumDef: Option[EnumDef]) {
  def kind: String = if (enumDef.isDefined) "Enum" else if (isBool) "Bool" else if (signed) "SInt" else "UInt"

  /** What sites sharing an event name must agree on: an enum field's enum, not merely its width. */
  def typeName: String = enumDef.map(_.name).getOrElse(if (isBool) "Bool" else s"$kind<$width>")
}

/** Where a field's value comes from when a record is decoded. */
private[eventtrace] sealed trait FieldSource

/** Carried in the payload, starting at this bit. */
private[eventtrace] case class Packed(offset: Int) extends FieldSource

/** Captured from a literal, so never transmitted; the decoder supplies it. */
private[eventtrace] case class Constant(value: BigInt) extends FieldSource

/** Left `DontCare` at this site, so absent from its records. */
private[eventtrace] case object Absent extends FieldSource

/** One field of a site. An event-local field left `DontCare` has no type until [[EventSchema.resolve]] takes it from
  * a site sharing its event name.
  */
private[eventtrace] case class FieldLayout(name: String, tpe: Option[FieldType], source: FieldSource) {
  private def resolved: FieldType = tpe.getOrElse(throw new IllegalStateException(s"field $name has no type yet"))
  def width: Int = resolved.width
  def signed: Boolean = resolved.signed
  def isBool: Boolean = resolved.isBool
  def enumDef: Option[EnumDef] = resolved.enumDef
  def kind: String = resolved.kind
  def typeName: String = tpe.map(_.typeName).getOrElse("DontCare")
}

/** One emission site, as recorded at elaboration, in the module instance that declared it. Its sink carries
  * `payloadWords` 32-bit words.
  *
  * @param written   the event name as written at the site, such as `arrive`
  * @param subject   the subject's object type, such as `packet`, for an event with a subject
  * @param lifecycle `create` for a create event
  */
private[eventtrace] case class SiteLayout(
  id:           Int,
  written:      String,
  subject:      Option[String],
  lifecycle:    Option[String],
  location:     String,
  fields:       Seq[FieldLayout],
  payloadWords: Int,
  module:       BaseModule) {

  /** The event's name as consumers see it: `packet.arrive` for an event with a subject, else as written. */
  def name: String = SiteLayout.qualifiedName(subject, written)

  /** The declaring module's Chisel instance path, such as `ChipTop.tile.dcache`; valid once elaboration has finished,
    * when every parent has named its children.
    */
  def instance: String = module.pathName
}

private[eventtrace] object SiteLayout {
  def qualifiedName(subject: Option[String], written: String): String = subject.fold(written)(s => s"$s.$written")
}

/** The format consumers see for one event name, whichever site emitted it: its fields, sorted by name, and which of
  * them some site leaves `DontCare`.
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

  /** Gives each event-local field left `DontCare` the type its namesake sites declare. Sites sharing a name were
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
        s"""field "$field" of event "$name" is DontCare at every site (${group.map(_.location).mkString(", ")}), so it has no type"""
      }
    }
    if (untyped.nonEmpty) Left(untyped)
    else Right(sites.map(s => s.copy(fields = s.fields.map(f => f.copy(tpe = Some(types(s.name)(f.name)))))))
  }

  /** Builds each event's schema from sites whose fields all have types. */
  def fromSites(sites: Seq[SiteLayout]): Seq[EventSchema] =
    sites.groupBy(_.name).toSeq.sortBy(_._1).map { case (name, group) =>
      val first = group.minBy(_.id)
      val optional = group.flatMap(_.fields).collect { case FieldLayout(field, _, Absent) => field }.toSet
      EventSchema(name, first.written, first.subject, first.lifecycle, first.fields.sortBy(_.name), optional, group.sortBy(_.id))
    }
}
