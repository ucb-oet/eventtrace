package eventtrace

import chisel3._
import chisel3.experimental.{BaseModule, SourceInfo, SourceLine}
import chisel3.internal.eventtrace.WhenDepth
import chisel3.util.Cat

import java.nio.file.Path
import scala.collection.mutable
import scala.util.DynamicVariable

/** The EventTrace frontend: events with primitive and enum fields, and objects created by create events.
  *
  * {{{
  * import eventtrace.EventTrace.syntax._
  * when (fire) {
  *   EventTrace.event("tick") { "count" := count }
  * }
  * when (arrive) {
  *   Packet.create(event = "arrive") { p => p.seq := seq; "lane" := lane }
  * }
  * }}}
  */
object EventTrace {
  type ObjectType = eventtrace.ObjectType
  type Schema[T <: eventtrace.ObjectType] = eventtrace.Schema[T]

  /** Each field decodes into a C++ scalar, so it is at most this wide; a payload may span any number of words. */
  val MaxFieldBits = 64

  /** Every record already has these keys, so no field may use them. */
  val ReservedFieldNames: Set[String] = Set("cycle", "event_id", "site_id", "subject")

  /** A field's value at one site. */
  private sealed trait Value

  /** Transmitted in the payload, already extended to the field's type. */
  private case class Hardware(value: Data) extends Value

  /** Known at elaboration, so supplied by the decoder. */
  private case class Literal(value: BigInt) extends Value

  /** Left `DontCare`. */
  private case object Unset extends Value

  /** One field written in the open event: an event-local capture, or an assignment to the subject's field. An
    * event-local `DontCare` has no type until generation.
    */
  private case class Entry(name: String, value: Value, tpe: Option[FieldType], objectField: Boolean)

  /** The event whose body is running, with the module and `when` depth in which it opened. */
  private case class OpenEvent(
    name:      String,
    subject:   Option[ObjectType],
    module:    BaseModule,
    whenDepth: Int,
    entries:   mutable.ArrayBuffer[Entry])

  private val openEvent = new DynamicVariable[Option[OpenEvent]](None)
  private val siteLayouts = mutable.ArrayBuffer.empty[SiteLayout]

  /** Every enum used so far, by simple name, which generated C++ and the manifest use. */
  private val enums = mutable.LinkedHashMap.empty[String, (ChiselEnum, EnumDef)]

  /** Every object type created so far: its field signature and where it was first created. */
  private val objectTypes = mutable.LinkedHashMap.empty[String, (Seq[String], String)]

  /** Emits a record on every clock edge at which the enclosing `when` conditions hold. */
  def event(name: String)(body: => Unit)(implicit sourceInfo: SourceInfo): Unit = {
    val what = s"""EventTrace.event("$name")"""
    checkEventName(what, name, sourceInfo)
    if (CppGen.ReservedEventNames.contains(name))
      elaborationError(s"$what: the name is reserved in the generated C++", sourceInfo)
    if (objectTypes.contains(name))
      elaborationError(
        s"$what: an object type is also named $name, and the generated C++ would give both one name",
        sourceInfo
      )
    site(what, name, None, sourceInfo)(body)
  }

  /** Runs a create block for `handle`; see [[Schema.create]]. */
  private[eventtrace] def createSite(handle: ObjectType, event: String, sourceInfo: SourceInfo)(body: => Unit): Unit = {
    val what = s"""create of "${handle.typeName}.$event""""
    checkEventName(what, event, sourceInfo)
    registerObjectType(handle, sourceInfo)
    handle.scope = ObjectType.Open
    try site(what, event, Some(handle), sourceInfo)(body)
    finally handle.scope = ObjectType.Closed
  }

  /** Captures a named event-local value; equivalent to `name := value`. */
  def capture(name: String, value: Bits)(implicit sourceInfo: SourceInfo): Unit =
    addCapture(name, Some(value), None, sourceInfo)

  /** Captures a `ChiselEnum` value; consumers see its variant names. The compiler finds its enum from its type. */
  def capture[T <: EnumType](name: String, value: T)(implicit ev: EnumOf[T], sourceInfo: SourceInfo): Unit =
    addCapture(name, Some(value), Some(ev.enumObject), sourceInfo)

  /** Declares a field this site has no value for, so that it matches the sites sharing its event name. The field
    * takes the type those sites declare, and is absent from this site's records.
    */
  def capture(name: String, value: DontCare.type)(implicit sourceInfo: SourceInfo): Unit =
    addCapture(name, None, None, sourceInfo)

  /** Writes the generated C++ and the manifest for every site elaborated so far into `dir`.
    *
    * Call it after elaboration has finished: sites' instance paths are only known then, and an event-local field
    * left `DontCare` takes its type from sites that may elaborate after it.
    */
  def emitGenerated(dir: String): Seq[Path] = {
    val sites = EventSchema.resolve(siteLayouts.toSeq) match {
      case Right(typed)   => typed
      case Left(untyped) => throw new IllegalArgumentException(untyped.mkString("; "))
    }
    Seq(CppGen.write(dir, sites), ManifestGen.write(dir, EventSchema.fromSites(sites), sites))
  }

  object syntax {
    implicit class CaptureName(private val name: String) extends AnyVal {
      def :=(value: Bits)(implicit sourceInfo: SourceInfo): Unit = capture(name, value)
      def :=[T <: EnumType](value: T)(implicit ev: EnumOf[T], sourceInfo: SourceInfo): Unit = capture(name, value)
      def :=(value: DontCare.type)(implicit sourceInfo: SourceInfo): Unit = capture(name, value)
    }
  }

  /** One emission site: runs `body` as the open event, then lays out its payload and instantiates its sink. */
  private def site(what: String, written: String, subject: Option[ObjectType], sourceInfo: SourceInfo)(body: => Unit): Unit = {
    if (openEvent.value.isDefined) elaborationError(s"$what is nested inside another event", sourceInfo)
    val name = SiteLayout.qualifiedName(subject.map(_.typeName), written)
    val lifecycle = subject.map(_ => "create")

    // The library call does not inherit the `when` the way a printf does; capture it explicitly.
    val enable = when.cond
    val module = Module.currentModule.getOrElse(elaborationError(s"$what is outside a module", sourceInfo))
    val entries = mutable.ArrayBuffer.empty[Entry]
    openEvent.withValue(Some(OpenEvent(name, subject, module, WhenDepth(), entries)))(body)

    subject.foreach { handle =>
      val missing = handle.fields.filter(_.property == FieldProperty.Const).map(_.name).filterNot { field =>
        entries.exists(e => e.objectField && e.name == field && e.value != Unset)
      }
      if (missing.nonEmpty)
        elaborationError(
          s"$what does not initialize const field(s) ${missing.mkString(", ")}; DontCare does not count",
          sourceInfo
        )
    }

    // Transmitted fields are laid out in declaration order, starting at the least significant bit.
    val packed = entries.collect { case Entry(field, Hardware(value), Some(t), _) => (field, value, t.width) }
    val offsets = packed.map(_._1).zip(packed.scanLeft(0)(_ + _._3)).toMap
    val payloadWords = (packed.map(_._3).sum + 31) / 32
    val fields = entries.toSeq.map { e =>
      val source = e.value match {
        case Hardware(_)    => Packed(offsets(e.name))
        case Literal(value) => Constant(value)
        case Unset          => Absent
      }
      FieldLayout(e.name, e.tpe, source)
    }
    checkNamesakes(what, name, lifecycle, fields, sourceInfo)

    val siteId = siteLayouts.length
    val sink = Module(new EventTraceSink(siteId, payloadWords)).suggestName(s"eventtrace_${name.replace('.', '_')}")
    sink.clock := Module.clock
    sink.reset := Module.reset.asBool
    sink.enable := enable
    sink.payload.foreach(_ := Cat(packed.reverse.map(_._2.asUInt).toSeq))
    siteLayouts += SiteLayout(siteId, written, subject.map(_.typeName), lifecycle, location(sourceInfo), fields, payloadWords, module)

    val summary = fields.map { f =>
      f.source match {
        case Packed(offset)  => s"${f.name}@$offset:${f.width}"
        case Constant(value) => s"${f.name}=$value"
        case Absent          => s"${f.name}=DontCare"
      }
    }.mkString(", ")
    println(s"""[eventtrace] site $siteId "$name" at ${location(sourceInfo)}: [$summary] in $payloadWords payload words""")
  }

  private def addCapture(name: String, value: Option[Data], enumObject: Option[ChiselEnum], sourceInfo: SourceInfo): Unit = {
    val what = s"""capture("$name")"""
    val open = openEvent.value.getOrElse(elaborationError(s"$what is outside an EventTrace.event block", sourceInfo))
    checkScope(what, open, sourceInfo)
    CppGen.identifierError(name).foreach(reason => elaborationError(s"$what: $reason", sourceInfo))
    if (ReservedFieldNames.contains(name))
      elaborationError(
        s"$what uses a reserved name; every record already has ${ReservedFieldNames.toSeq.sorted.mkString(", ")}",
        sourceInfo
      )
    open.entries.find(_.name == name).foreach { e =>
      if (e.objectField) elaborationError(s"$what has the name of a field this block assigns to its subject", sourceInfo)
      else elaborationError(s"$what repeats a field of the same event", sourceInfo)
    }
    open.entries += (value match {
      case None => Entry(name, Unset, None, objectField = false)
      case Some(v) =>
        val tpe = FieldType(knownWidth(what, v, sourceInfo), v.isInstanceOf[SInt], v.isInstanceOf[Bool],
          enumObject.map(registerEnum(_, sourceInfo)))
        Entry(name, v.litOption.map(Literal).getOrElse(Hardware(v)), Some(tpe), objectField = false)
    })
  }

  /** Records an assignment to a field of the open event's subject; `None` is `DontCare`. */
  private[eventtrace] def assignField(field: ObjectField, value: Option[Data], sourceInfo: SourceInfo): Unit = {
    val what = s"${field.owner.typeName}.${field.name} :="
    field.owner.scope match {
      case ObjectType.Closed  => elaborationError(s"$what uses a handle after its create block closed", sourceInfo)
      case ObjectType.Unbound => elaborationError(s"$what uses an object that no create block produced", sourceInfo)
      case ObjectType.Open    =>
    }
    val open = openEvent.value.getOrElse(elaborationError(s"$what is outside an event block", sourceInfo))
    if (!open.subject.exists(_ eq field.owner))
      elaborationError(s"$what assigns a field of an object that is not the subject of the open event", sourceInfo)
    checkScope(what, open, sourceInfo)
    open.entries.find(_.name == field.name).foreach { e =>
      if (e.objectField) elaborationError(s"$what assigns the field a second time in one block", sourceInfo)
      else elaborationError(s"$what has the name of an event field this block captures", sourceInfo)
    }
    open.entries += (value match {
      case None => Entry(field.name, Unset, Some(field.tpe), objectField = true)
      case Some(v) =>
        val extended = extend(what, field.tpe, v, sourceInfo)
        Entry(field.name, v.litOption.map(Literal).getOrElse(Hardware(extended)), Some(field.tpe), objectField = true)
    })
  }

  /** `value` extended to an object field's type. Truncation and mixing `UInt` with `SInt` are errors; `Bool` and
    * `UInt<1>` convert both ways. An enum field's static type already admits only its own enum.
    */
  private def extend(what: String, t: FieldType, value: Data, sourceInfo: SourceInfo): Data = {
    val width = knownWidth(what, value, sourceInfo)
    def truncation = elaborationError(
      s"$what assigns a $width-bit value to a ${t.typeName} field; truncate it explicitly, as in x(${t.width - 1}, 0)",
      sourceInfo
    )
    def mixing(kind: String) = elaborationError(s"$what assigns a $kind value to a ${t.typeName} field", sourceInfo)
    if (t.enumDef.isDefined) value
    else if (t.isBool) value match {
      case b: Bool                => b
      case u: UInt if width == 1 => u.asBool
      case _: UInt                => truncation
      case _                      => mixing("SInt")
    }
    else if (t.signed) value match {
      case s: SInt if width <= t.width => s.pad(t.width)
      case _: SInt                     => truncation
      case _                           => mixing("UInt")
    }
    else value match {
      case u: UInt if width <= t.width => u.asUInt.pad(t.width)
      case _: UInt                     => truncation
      case _                           => mixing("SInt")
    }
  }

  /** The type of an object field declared from `gen`. */
  private[eventtrace] def typeOf(gen: Data, enumObject: Option[ChiselEnum], name: String, sourceInfo: SourceInfo): FieldType = {
    val what = s"object field $name"
    CppGen.identifierError(name).foreach(reason => elaborationError(s"$what: $reason", sourceInfo))
    if (ReservedFieldNames.contains(name))
      elaborationError(
        s"$what uses a reserved name; every record already has ${ReservedFieldNames.toSeq.sorted.mkString(", ")}",
        sourceInfo
      )
    FieldType(knownWidth(what, gen, sourceInfo), gen.isInstanceOf[SInt], gen.isInstanceOf[Bool],
      enumObject.map(registerEnum(_, sourceInfo)))
  }

  /** A capture or assignment must be in the event's own module, and under no `when` nested inside the event. */
  private def checkScope(what: String, open: OpenEvent, sourceInfo: SourceInfo): Unit = {
    // The sink lives in the event's module, so it cannot reach a value inside a child module's constructor.
    if (!Module.currentModule.exists(_ eq open.module))
      elaborationError(
        s"""$what is in a different module from the event "${open.name}", """ +
          "for example in a child module elaborated inside the event body; capture in the event's own module",
        sourceInfo
      )
    // The payload is connected in the event's own scope, so a nested `when` would not govern this field.
    if (WhenDepth() > open.whenDepth)
      elaborationError(
        s"""$what is under a `when` nested inside the event "${open.name}"; """ +
          "conditional fields are not supported, so use Mux or a separate event",
        sourceInfo
      )
  }

  private def knownWidth(what: String, value: Data, sourceInfo: SourceInfo): Int = {
    val width = value.widthOption.getOrElse(
      elaborationError(s"$what has an unknown width; slice or pad it to a known width", sourceInfo)
    )
    if (width > MaxFieldBits)
      elaborationError(
        s"$what is $width bits wide; a field decodes into a C++ scalar of at most $MaxFieldBits bits, " +
          "so split it into several fields",
        sourceInfo
      )
    width
  }

  private def checkEventName(what: String, name: String, sourceInfo: SourceInfo): Unit =
    CppGen.identifierError(name).foreach(reason => elaborationError(s"$what: $reason", sourceInfo))

  /** Object types become C++ namespaces, so their names follow the event-name rules; all instances of one object type
    * must declare the same fields, which matters for parameterized schemas.
    */
  private def registerObjectType(handle: ObjectType, sourceInfo: SourceInfo): Unit = {
    val name = handle.typeName
    val signature = handle.fields.map(f => s"${f.name}: ${f.tpe.typeName} ${f.property}")
    objectTypes.get(name) match {
      case Some((known, _)) if known == signature =>
      case Some((known, at)) =>
        elaborationError(
          s"object type $name declares [${signature.mkString(", ")}], but the instance created at $at declares " +
            s"[${known.mkString(", ")}]; all instances of an object type must declare the same fields",
          sourceInfo
        )
      case None =>
        CppGen.identifierError(name).foreach(reason => elaborationError(s"object type $name: $reason", sourceInfo))
        if (CppGen.ReservedEventNames.contains(name))
          elaborationError(s"object type $name: the name is reserved in the generated C++", sourceInfo)
        siteLayouts.find(s => s.subject.isEmpty && s.written == name).foreach { other =>
          elaborationError(
            s"""object type $name has the name of the event at ${other.location}, and the generated C++ would give """ +
              "both one name",
            sourceInfo
          )
        }
        objectTypes(name) = (signature, location(sourceInfo))
    }
  }

  /** Generated C++ and the manifest name an enum by its simple name, so two different enums may not share one. */
  private def registerEnum(e: ChiselEnum, sourceInfo: SourceInfo): EnumDef = {
    val d = EnumDef(e)
    enums.get(d.name) match {
      case Some((other, known)) if other ne e =>
        elaborationError(
          s"two different ChiselEnums are named ${d.name}: ${known.fullName} and ${d.fullName}; enum names must be unique",
          sourceInfo
        )
      case Some((_, known)) => known
      case None =>
        CppGen.identifierError(d.name).foreach(reason => elaborationError(s"ChiselEnum ${d.fullName}: $reason", sourceInfo))
        d.variants.foreach { case (variant, _) =>
          CppGen.identifierError(variant).foreach { reason =>
            elaborationError(s"ChiselEnum ${d.fullName} variant $variant: $reason", sourceInfo)
          }
        }
        enums(d.name) = (e, d)
        d
    }
  }

  /** Sites sharing a name are one event to consumers. They must declare the same fields, in any order, with the
    * same types where both declare one, since an event-local `DontCare` has none; and agree on creating the subject.
    */
  private def checkNamesakes(
    what:       String,
    name:       String,
    lifecycle:  Option[String],
    fields:     Seq[FieldLayout],
    sourceInfo: SourceInfo
  ): Unit =
    siteLayouts.filter(_.name == name).foreach { other =>
      def signature(fs: Seq[FieldLayout]) = fs.map(f => s"${f.name}: ${f.typeName}").sorted.mkString("[", ", ", "]")
      val conflicting = fields.map(_.name).toSet != other.fields.map(_.name).toSet || fields.exists { f =>
        other.fields.find(_.name == f.name).exists(o => f.tpe.isDefined && o.tpe.isDefined && f.tpe != o.tpe)
      }
      if (conflicting)
        elaborationError(
          s"$what declares ${signature(fields)}, but the site with the same name at ${other.location} declares " +
            s"${signature(other.fields)}; sites sharing an event name must declare exactly the same fields",
          sourceInfo
        )
      if (lifecycle != other.lifecycle)
        elaborationError(
          s"$what and the site with the same name at ${other.location} disagree on creating their subject; " +
            "sites sharing an event name must agree on lifecycle",
          sourceInfo
        )
    }

  private def location(sourceInfo: SourceInfo): String = sourceInfo match {
    case SourceLine(file, line, _) => s"$file:$line"
    case _                         => "unknown location"
  }

  private def elaborationError(message: String, sourceInfo: SourceInfo): Nothing =
    throw new IllegalArgumentException(s"$message (${location(sourceInfo)})")
}
