package eventtrace

import chisel3._
import chisel3.experimental.{BaseModule, SourceInfo, SourceLine}
import chisel3.internal.eventtrace.{Elaboration, WhenDepth}
import chisel3.util.Cat
import firrtl.transforms.BlackBoxInlineAnno

import scala.collection.mutable
import scala.util.DynamicVariable

/** The EventTrace frontend: events with primitive and enum fields, objects, collections, and queries.
  *
  * {{{
  * import eventtrace.EventTrace.syntax._
  * when (fire) {
  *   EventTrace.event("tick") { "count" := count }
  * }
  * when (enterDecode) {
  *   Instruction.create(event = "decode") { i => i.pc := pc; i.stage := Stage.Decode; active.add(i) }
  * }
  * when (retire) {
  *   val i = active.head
  *   i.event("retire") { active.remove(i); i.destroy() }
  * }
  * }}}
  */
object EventTrace {
  type ObjectType = eventtrace.ObjectType
  type Schema[T <: eventtrace.ObjectType] = eventtrace.Schema[T]
  type Collection[T <: eventtrace.ObjectType] = eventtrace.Collection[T]

  /** Each field decodes into a C++ scalar, so it is at most this wide; a payload may span any number of words. */
  val MaxFieldBits = 64

  /** The runtime marks the fields assigned to an object in one cycle in a 64-bit mask. */
  val MaxObjectFields = 64

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

  /** The ID of the object `handle` resolves to, supplied by the runtime. */
  private case class Ref(handle: ObjectType) extends Value

  /** One field written in the open event: an event-local capture, or an assignment to the subject's `field`. An
    * event-local `DontCare` has no type until generation.
    */
  private case class Entry(name: String, value: Value, tpe: Option[FieldType], field: Option[ObjectField]) {
    def objectField: Boolean = field.isDefined
  }

  /** The event whose body is running, with the module and `when` depth in which it opened. */
  private case class OpenEvent(
    name:      String,
    subject:   Option[ObjectType],
    created:   Boolean,
    module:    BaseModule,
    whenDepth: Int,
    entries:   mutable.ArrayBuffer[Entry],
    effects:   mutable.ArrayBuffer[Effect])

  private val openEvent = new DynamicVariable[Option[OpenEvent]](None)

  /** The member handle of the `each` block whose body is running. */
  private val openEach = new DynamicVariable[Option[ObjectType]](None)

  /** Set while an event reads its enable from `when.cond`, which evaluates every enclosing condition again. */
  private[eventtrace] val computingEnable = new DynamicVariable(false)

  /** What one elaboration has declared. A program may elaborate several designs, as the rejection tests do; each
    * starts afresh, so its site IDs count from 0 and its generated files describe only its own sites.
    */
  private class Declarations(val elaboration: AnyRef) {
    val siteLayouts = mutable.ArrayBuffer.empty[SiteLayout]

    /** Every enum used so far, by simple name, which generated C++ and the manifest use. */
    val enums = mutable.LinkedHashMap.empty[String, (ChiselEnum, EnumDef)]

    /** Every object type used so far: its fields, their signature, and where it was first used. */
    val objectTypes = mutable.LinkedHashMap.empty[String, ObjectTypeInfo]

    val collections = mutable.ArrayBuffer.empty[CollectionInfo]

    /** Whether a sink carries the generated files yet; see [[carryFiles]]. */
    var carried = false
  }

  private var latest: Option[Declarations] = None

  /** The current elaboration's declarations. */
  private def declarations: Declarations = {
    val elaboration = Elaboration.current()
    latest.filter(_.elaboration eq elaboration).getOrElse {
      val fresh = new Declarations(elaboration)
      latest = Some(fresh)
      fresh
    }
  }

  private def siteLayouts = declarations.siteLayouts
  private def enums = declarations.enums
  private def objectTypes = declarations.objectTypes
  private def collections = declarations.collections

  private var tracing = false

  /** Turns tracing on, or off with `on = false`, for what is elaborated next.
    *
    * Tracing is off until a flow turns it on, so generators can carry annotations at no cost to builds that do not
    * trace: event and create blocks are not even run, and no sink is built. Annotation mistakes therefore surface
    * only in traced builds. In Chipyard, a config with `WithEventTrace` turns tracing on.
    */
  def enable(on: Boolean = true): Unit = tracing = on

  /** Emits a record on every clock edge at which the enclosing `when` conditions hold. */
  def event(name: String)(body: => Unit)(implicit sourceInfo: SourceInfo): Unit = if (tracing) {
    val what = s"""EventTrace.event("$name")"""
    checkEventName(what, name, sourceInfo)
    if (CppGen.ReservedEventNames.contains(name))
      elaborationError(s"$what: the name is reserved in the generated C++", sourceInfo)
    if (objectTypes.contains(name))
      elaborationError(
        s"$what: an object type is also named $name, and the generated C++ would give both one name",
        sourceInfo
      )
    site(what, name, None, created = false, sourceInfo)(body)
  }

  /** Declares a collection of objects of `gen`'s type in the current module. Each elaborated declaration is its own
    * collection; modules share one by passing it, as a constructor parameter or through a `Parameters` key bound in
    * the parent.
    */
  def collection[T <: ObjectType](gen: => T)(implicit name: sourcecode.Name, sourceInfo: SourceInfo): Collection[T] = {
    val module = Module.currentModule.getOrElse(
      elaborationError(
        s"collection ${name.value} is declared outside a module; declare it in a module and pass it to the others",
        sourceInfo
      )
    )
    val sample = gen
    registerObjectType(sample, sourceInfo)
    val info = new CollectionInfo(collections.length, sample.typeName, module, name.value)
    collections += info
    new Collection(info, () => gen)
  }

  /** Runs a create block for `handle`; see [[Schema.create]]. */
  private[eventtrace] def createSite(handle: ObjectType, event: String, sourceInfo: SourceInfo)(body: => Unit): Unit = if (tracing) {
    val what = s"""create of "${handle.typeName}.$event""""
    checkEventName(what, event, sourceInfo)
    registerObjectType(handle, sourceInfo)
    handle.binding = Binding.Created(open = true)
    try site(what, event, Some(handle), created = true, sourceInfo)(body)
    finally handle.binding = Binding.Created(open = false)
  }

  /** Runs an event block whose subject is an existing object; see [[ObjectType.event]]. */
  private[eventtrace] def objectEvent(handle: ObjectType, event: String, sourceInfo: SourceInfo)(body: => Unit): Unit = if (tracing) {
    val what = s"""${handle.typeName}.event("$event")"""
    checkEventName(what, event, sourceInfo)
    checkHandle(what, handle, sourceInfo)
    registerObjectType(handle, sourceInfo)
    site(what, event, Some(handle), created = false, sourceInfo)(body)
  }

  /** Runs an `each` block with `member` standing for every member of `view`; see [[View.each]]. */
  private[eventtrace] def each[T <: ObjectType](view: ViewExpr, member: T, sourceInfo: SourceInfo)(body: T => Unit): Unit = if (tracing) {
    if (openEach.value.isDefined) elaborationError("each is nested inside another each block, which is not supported", sourceInfo)
    if (openEvent.value.isDefined)
      elaborationError("each is inside an event block; put the events inside the each block instead", sourceInfo)
    member.binding = Binding.EachMember(view, open = true)
    try openEach.withValue(Some(member))(body(member))
    finally member.binding = Binding.EachMember(view, open = false)
  }

  /** Captures a named event-local value; equivalent to `name := value`. */
  def capture(name: String, value: Bits)(implicit sourceInfo: SourceInfo): Unit =
    addCapture(name, sourceInfo)(what => dataEntry(what, name, value, None, sourceInfo))

  /** Captures a `ChiselEnum` value; consumers see its variant names. The compiler finds its enum from its type. */
  def capture[T <: EnumType](name: String, value: T)(implicit ev: EnumOf[T], sourceInfo: SourceInfo): Unit =
    addCapture(name, sourceInfo)(what => dataEntry(what, name, value, Some(ev.enumObject), sourceInfo))

  /** Captures an object's ID, which the runtime resolves; the record prints it as `type#id`. */
  def capture(name: String, id: IdRef)(implicit sourceInfo: SourceInfo): Unit =
    addCapture(name, sourceInfo) { what =>
      checkHandle(what, id.handle, sourceInfo)
      Entry(name, Ref(id.handle), Some(FieldType.id(id.handle.typeName)), None)
    }

  /** Captures an object's ID; equivalent to `capture(name, handle.id)`. */
  def capture(name: String, handle: ObjectType)(implicit sourceInfo: SourceInfo): Unit = capture(name, handle.id)

  /** Declares a field this site has no value for, so that it matches the sites sharing its event name. The field
    * takes the type those sites declare, and is absent from this site's records.
    */
  def capture(name: String, value: DontCare.type)(implicit sourceInfo: SourceInfo): Unit =
    addCapture(name, sourceInfo)(_ => Entry(name, Unset, None, None))

  /** The manifest of the most recent elaboration, as JSON: what consumers see, for tools outside the simulator.
    *
    * It describes the design rather than being compiled into the simulator, so each flow writes it with its other
    * descriptions of the design once elaboration has finished. Chipyard registers it with rocket-chip's
    * `ElaborationArtefacts`, which builds each file after elaboration and writes it next to the `.dts`; other flows
    * write it after elaborating. Sites' instance paths, and the types of event-local fields left `DontCare` at some
    * sites, are only known once elaboration has finished.
    */
  def manifest: String = {
    val sites = latest.map(resolvedSites).getOrElse(Seq.empty)
    ManifestGen.json(EventSchema.fromSites(sites), sites)
  }

  /** Every site of an elaboration, each event-local field left `DontCare` taking its type from the other sites. */
  private def resolvedSites(d: Declarations): Seq[SiteLayout] =
    EventSchema.resolve(d.siteLayouts.toSeq) match {
      case Right(typed)   => typed
      case Left(untyped) => throw new IllegalArgumentException(untyped.mkString("; "))
    }

  /** The runtime and DPI adapter, shipped in the frontend's resources. */
  private val RuntimeSources = Seq("eventtrace_runtime.h", "eventtrace_runtime.cc", "eventtrace_tracker.h", "eventtrace_dpi.cc")

  /** Makes the elaboration's first sink carry everything the simulator compiles besides the RTL: the runtime sources
    * and the generated C++. firtool writes them next to the RTL, as it does any black box's files, so no step has to
    * run after elaboration, in Chipyard or elsewhere.
    *
    * The generated C++ must wait until elaboration has finished, for the same reasons as [[manifest]]. Chisel builds
    * an annotation's contents only then, when it converts the finished circuit.
    */
  private def carryFiles(sink: EventTraceSink, d: Declarations): Unit = {
    RuntimeSources.foreach(f => sink.addResource(s"/eventtrace/csrc/$f"))
    chisel3.experimental.annotate(sink) {
      val sites = resolvedSites(d)
      val manifestSha256 = ManifestGen.sha256(ManifestGen.json(EventSchema.fromSites(sites), sites))
      val header = CppGen.header(sites, d.objectTypes.values.toSeq, d.collections.toSeq, manifestSha256)
      Seq(BlackBoxInlineAnno(sink.toNamed, CppGen.FileName, header))
    }
  }

  object syntax {
    implicit class CaptureName(private val name: String) extends AnyVal {
      def :=(value: Bits)(implicit sourceInfo: SourceInfo): Unit = capture(name, value)
      def :=[T <: EnumType](value: T)(implicit ev: EnumOf[T], sourceInfo: SourceInfo): Unit = capture(name, value)
      def :=(value: IdRef)(implicit sourceInfo: SourceInfo): Unit = capture(name, value)
      def :=(value: ObjectType)(implicit sourceInfo: SourceInfo): Unit = capture(name, value)
      def :=(value: DontCare.type)(implicit sourceInfo: SourceInfo): Unit = capture(name, value)
    }
  }

  /** One emission site: runs `body` as the open event, then lays out its payload and instantiates its sink. */
  private def site(what: String, written: String, subject: Option[ObjectType], created: Boolean, sourceInfo: SourceInfo)(
    body: => Unit
  ): Unit = {
    if (openEvent.value.isDefined) elaborationError(s"$what is nested inside another event", sourceInfo)
    openEach.value.foreach { member =>
      if (!subject.exists(_ eq member))
        elaborationError(s"$what is inside an each block, which may hold only events on its member", sourceInfo)
    }
    val name = SiteLayout.qualifiedName(subject.map(_.typeName), written)

    // The library call does not inherit the `when` the way a printf does; capture it explicitly. `when.cond`
    // evaluates each enclosing condition again, so a condition that builds state builds it twice; Utils' edges
    // reject being evaluated again.
    val enable = computingEnable.withValue(true)(when.cond)
    val module = Module.currentModule.getOrElse(elaborationError(s"$what is outside a module", sourceInfo))
    val open = OpenEvent(name, subject, created, module, WhenDepth(), mutable.ArrayBuffer.empty, mutable.ArrayBuffer.empty)
    openEvent.withValue(Some(open))(body)
    val entries = open.entries
    val lifecycle = if (created) Some("create") else if (open.effects.contains(Effect.Destroy)) Some("destroy") else None

    if (created) subject.foreach { handle =>
      val missing = handle.fields.filter(_.property == FieldProperty.Const).map(_.name).filterNot { field =>
        entries.exists(e => e.objectField && e.name == field && e.value != Unset)
      }
      if (missing.nonEmpty)
        elaborationError(
          s"$what does not initialize const field(s) ${missing.mkString(", ")}; DontCare does not count",
          sourceInfo
        )
    }

    // Transmitted fields are laid out in declaration order, starting at the least significant bit, followed by the
    // hardware values the site's queries compare with.
    val refs = entries.collect { case Entry(_, Ref(handle), _, _) => handle }
    val resolved = Option.when(subject.isDefined || refs.nonEmpty)(handles(what, open, subject.toSeq ++ refs, sourceInfo))
    val queryValues = resolved.toSeq.flatMap(_._2)
    val packed = entries.collect { case Entry(field, Hardware(value), Some(t), _) => (field, value, t.width) }
    val offsets = packed.map(_._1).zip(packed.scanLeft(0)(_ + _._3)).toMap
    val operandOffsets = queryValues.zip(queryValues.scanLeft(packed.map(_._3).sum)(_ + _.tpe.width))
    val payloadBits = packed.map(_._3).sum + queryValues.map(_.tpe.width).sum
    val payloadWords = (payloadBits + 31) / 32
    val fields = entries.toSeq.map { e =>
      val source = e.value match {
        case Hardware(_)    => Packed(offsets(e.name))
        case Literal(value) => Constant(value)
        case Unset          => Absent
        case Ref(handle)    => Reference(handle)
      }
      FieldLayout(e.name, e.tpe, source)
    }
    checkNamesakes(what, name, lifecycle, fields, sourceInfo)

    val plan = resolved.map { case (handlePlans, _) =>
      SitePlan(
        subject.map(s => handlePlans.find(_.handle eq s).get),
        handlePlans,
        operandOffsets,
        entries.toSeq.collect { case Entry(_, value, _, Some(field)) if value != Unset => field },
        open.effects.toSeq
      )
    }
    val siteId = siteLayouts.length
    val sink = Module(new EventTraceSink(siteId, payloadWords)).suggestName(s"eventtrace_${name.replace('.', '_')}")
    sink.clock := Module.clock
    sink.reset := Module.reset.asBool
    sink.enable := enable
    val transmitted = packed.map(_._2.asUInt) ++ queryValues.map(_.value.asUInt)
    sink.payload.foreach(_ := Cat(transmitted.reverse.toSeq))
    val d = declarations
    if (!d.carried) {
      d.carried = true
      carryFiles(sink, d)
    }
    siteLayouts += SiteLayout(siteId, written, subject.map(_.typeName), lifecycle, location(sourceInfo), fields, payloadWords,
      module, plan)

    val summary = fields.map { f =>
      f.source match {
        case Packed(offset)  => s"${f.name}@$offset:${f.width}"
        case Constant(value) => s"${f.name}=$value"
        case Absent          => s"${f.name}=DontCare"
        case Reference(_)    => s"${f.name}=id"
      }
    } ++ operandOffsets.map { case (op, offset) => s"query@$offset:${op.tpe.width}" }
    println(s"""[eventtrace] site $siteId "$name" at ${location(sourceInfo)}: [${summary.mkString(", ")}] in $payloadWords payload words""")
  }

  /** Every handle a site resolves, starting from its subject and the handles whose IDs it captures and following
    * the queries they come from, in construction order; and the hardware values those queries compare with.
    */
  private def handles(what: String, open: OpenEvent, roots: Seq[ObjectType], sourceInfo: SourceInfo)
    : (Seq[HandlePlan], Seq[Operand.Hardware]) = {
    val seen = mutable.LinkedHashMap.empty[Long, ObjectType]
    def visit(handle: ObjectType): Unit = if (!seen.contains(handle.serial)) {
      seen(handle.serial) = handle
      checkHandle(what, handle, sourceInfo)
      viewOf(handle).foreach(view => Queries.handles(view).foreach(visit))
    }
    roots.foreach(visit)
    val ordered = seen.values.toSeq.sortBy(_.serial)
    val values = mutable.ArrayBuffer.empty[Operand.Hardware]
    ordered.flatMap(viewOf).flatMap(Queries.hardware).foreach { v => if (!values.exists(_ eq v)) values += v }
    values.foreach { v =>
      if (!(v.module eq open.module))
        elaborationError(
          s"$what uses a query comparing with a hardware value from another module (at ${v.location}); a query's " +
            "hardware values count as captures of the events using it, so they must be in the event's module",
          sourceInfo
        )
    }
    (ordered.map(h => HandlePlan(h, h.binding)), values.toSeq)
  }

  private def viewOf(handle: ObjectType): Option[ViewExpr] = handle.binding match {
    case Binding.Chosen(view, _, _)   => Some(view)
    case Binding.EachMember(view, _) => Some(view)
    case _                            => None
  }

  /** A handle may be used where its object can be resolved: inside its create or `each` block, or anywhere if a
    * chooser produced it.
    */
  private def checkHandle(what: String, handle: ObjectType, sourceInfo: SourceInfo): Unit = handle.binding match {
    case Binding.Chosen(_, _, _) | Binding.Created(true) | Binding.EachMember(_, true) =>
    case Binding.Created(false) => elaborationError(s"$what uses a handle after its create block closed", sourceInfo)
    case Binding.EachMember(_, false) =>
      elaborationError(s"$what uses an each member after its each block closed", sourceInfo)
    case Binding.Member(_) =>
      elaborationError(s"$what uses the member of a where or orderBy outside its predicate", sourceInfo)
    case Binding.Unbound =>
      elaborationError(s"$what uses an object that no create block or query produced", sourceInfo)
  }

  private def addCapture(name: String, sourceInfo: SourceInfo)(entry: String => Entry): Unit = {
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
    open.entries += entry(what)
  }

  private def dataEntry(what: String, name: String, v: Data, enumObject: Option[ChiselEnum], sourceInfo: SourceInfo): Entry = {
    val tpe = FieldType(knownWidth(what, v, sourceInfo), v.isInstanceOf[SInt], v.isInstanceOf[Bool],
      enumObject.map(registerEnum(_, sourceInfo)))
    Entry(name, v.litOption.map(Literal).getOrElse(Hardware(v)), Some(tpe), None)
  }

  /** Records an assignment to a field of the open event's subject; `None` is `DontCare`. */
  private[eventtrace] def assignField(field: ObjectField, value: Option[Data], sourceInfo: SourceInfo): Unit = {
    val what = s"${field.owner.typeName}.${field.name} :="
    checkHandle(what, field.owner, sourceInfo)
    val open = subjectEvent(what, field.owner, sourceInfo)
    if (field.property == FieldProperty.Const && !open.created && value.isDefined)
      elaborationError(s"$what assigns a const field outside its object's create block", sourceInfo)
    open.entries.find(_.name == field.name).foreach { e =>
      if (e.objectField) elaborationError(s"$what assigns the field a second time in one block", sourceInfo)
      else elaborationError(s"$what has the name of an event field this block captures", sourceInfo)
    }
    open.entries += (value match {
      case None => Entry(field.name, Unset, Some(field.tpe), Some(field))
      case Some(v) =>
        val extended = extend(what, field.tpe, v, sourceInfo)
        Entry(field.name, v.litOption.map(Literal).getOrElse(Hardware(extended)), Some(field.tpe), Some(field))
    })
  }

  /** Records `add` or `remove` on the open event's subject. */
  private[eventtrace] def effect(effect: Effect, handle: ObjectType, what: String, sourceInfo: SourceInfo): Unit = {
    checkHandle(what, handle, sourceInfo)
    subjectEvent(what, handle, sourceInfo).effects += effect
  }

  /** Records the destruction of the open event's subject. */
  private[eventtrace] def destroy(handle: ObjectType, sourceInfo: SourceInfo): Unit = {
    val what = s"${handle.typeName}.destroy()"
    checkHandle(what, handle, sourceInfo)
    val open = subjectEvent(what, handle, sourceInfo)
    if (open.created)
      elaborationError(s"$what is in its subject's create block; no event both creates and destroys its subject", sourceInfo)
    if (open.effects.contains(Effect.Destroy)) elaborationError(s"$what destroys the subject a second time", sourceInfo)
    open.effects += Effect.Destroy
  }

  /** The open event, if `handle` is its subject: every effect acts on its event's subject. */
  private def subjectEvent(what: String, handle: ObjectType, sourceInfo: SourceInfo): OpenEvent = {
    val open = openEvent.value.getOrElse(elaborationError(s"$what is outside an event block", sourceInfo))
    if (open.subject.isEmpty)
      elaborationError(s"""$what is in the event "${open.name}", which has no subject and so no effects""", sourceInfo)
    if (!open.subject.exists(_ eq handle))
      elaborationError(
        s"""$what acts on an object that is not the subject of the event "${open.name}"; every effect acts on """ +
          "its event's subject",
        sourceInfo
      )
    checkScope(what, open, sourceInfo)
    open
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

  /** The type of a value compared in a query: `known` for a value of an enum field's enum, else its own. */
  private[eventtrace] def valueType(what: String, value: Data, known: Option[FieldType], sourceInfo: SourceInfo): FieldType =
    known.getOrElse(FieldType(knownWidth(what, value, sourceInfo), value.isInstanceOf[SInt], value.isInstanceOf[Bool], None))

  /** A capture or effect must be in the event's own module, and under no `when` nested inside the event. */
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
          "conditional fields and effects are not supported, so use Mux or a separate event",
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
      case Some(known) if known.signature == signature =>
      case Some(known) =>
        elaborationError(
          s"object type $name declares [${signature.mkString(", ")}], but the instance first used at " +
            s"${known.location} declares [${known.signature.mkString(", ")}]; all instances of an object type must " +
            "declare the same fields",
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
        if (handle.fields.length > MaxObjectFields)
          elaborationError(
            s"object type $name declares ${handle.fields.length} fields; an object type has at most $MaxObjectFields",
            sourceInfo
          )
        objectTypes(name) = ObjectTypeInfo(name, handle.fields, signature, location(sourceInfo))
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
    * same types where both declare one, since an event-local `DontCare` has none; and agree on creating and on
    * destroying the subject.
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
          s"$what and the site with the same name at ${other.location} disagree on creating or destroying their " +
            "subject; sites sharing an event name must agree on lifecycle",
          sourceInfo
        )
    }

  private[eventtrace] def location(sourceInfo: SourceInfo): String = sourceInfo match {
    case SourceLine(file, line, _) => s"$file:$line"
    case _                         => "unknown location"
  }

  private[eventtrace] def elaborationError(message: String, sourceInfo: SourceInfo): Nothing =
    throw new IllegalArgumentException(s"$message (${location(sourceInfo)})")
}

/** An object type's declaration as first used: its fields, in declaration order, and their signature. */
private[eventtrace] case class ObjectTypeInfo(name: String, fields: Seq[ObjectField], signature: Seq[String], location: String)
