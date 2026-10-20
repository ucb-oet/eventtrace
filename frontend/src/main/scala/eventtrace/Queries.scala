package eventtrace

import chisel3._
import chisel3.experimental.{BaseModule, SourceInfo}

/** A collection declared in a module: a set of objects that records the order in which its members joined.
  *
  * {{{
  * val active = EventTrace.collection(new Instruction)
  * when (retire) {
  *   val i = active.head
  *   i.event("retire") { active.remove(i); i.complete() }
  * }
  * }}}
  *
  * A collection is itself a view in join order. `add` and `remove` are effects, legal only inside an event block on
  * the object they name.
  */
final class Collection[T <: ObjectType] private[eventtrace] (info: CollectionInfo, gen: () => T)
    extends View[T](ViewExpr.Source(info), gen) {

  /** The subject joins this collection, last in join order, when the open event fires. */
  def add(handle: T)(implicit sourceInfo: SourceInfo): Unit =
    EventTrace.effect(Effect.Add(info), handle, s"${info.valName}.add", sourceInfo)

  /** The subject leaves this collection when the open event fires. */
  def remove(handle: T)(implicit sourceInfo: SourceInfo): Unit =
    EventTrace.effect(Effect.Remove(info), handle, s"${info.valName}.remove", sourceInfo)
}

/** A pure query over a collection's members, evaluated by the runtime against the state before the clock edge.
  * Views filter and rank; choosers pick the objects an event concerns.
  */
class View[T <: ObjectType] private[eventtrace] (private[eventtrace] val expr: ViewExpr, gen: () => T) {

  /** The members satisfying `p`, in this view's order. `p` compares the member's fields and `id` with literals,
    * hardware values, and other handles' fields, using `=== =/= < <= > >= && || !`.
    */
  def where(p: T => Pred)(implicit sourceInfo: SourceInfo): View[T] = {
    val member = Queries.member(gen())
    val pred = try p(member) finally Queries.close(member)
    Queries.checkPred(pred, member, sourceInfo)
    new View(ViewExpr.Where(expr, member, pred), gen)
  }

  /** The members ranked by one key, a field or the `id` of the member, ascending; ties keep join order. A member
    * whose key is unset is a runtime violation.
    */
  def orderBy(key: T => Key)(implicit sourceInfo: SourceInfo): View[T] = {
    val member = Queries.member(gen())
    val k = try key(member).operand finally Queries.close(member)
    k match {
      case Operand.Field(h, _) if h eq member =>
      case Operand.Id(h) if h eq member       =>
      case _ => EventTrace.elaborationError("orderBy ranks by a field or the id of the member it receives", sourceInfo)
    }
    new View(ViewExpr.OrderBy(expr, member, k), gen)
  }

  /** The first member; a runtime violation if there is none. */
  def head(implicit sourceInfo: SourceInfo): T = choose(Chooser.Head, sourceInfo)

  /** The last member; a runtime violation if there is none. */
  def last(implicit sourceInfo: SourceInfo): T = choose(Chooser.Last, sourceInfo)

  /** Member `k`, counting from 0; a runtime violation if there are not that many. */
  def at(k: Int)(implicit sourceInfo: SourceInfo): T = {
    if (k < 0) EventTrace.elaborationError(s"at($k) needs a position of at least 0", sourceInfo)
    choose(Chooser.At(k), sourceInfo)
  }

  /** The one member; a runtime violation if there are none or several. */
  def only(implicit sourceInfo: SourceInfo): T = choose(Chooser.Only, sourceInfo)

  /** Runs `body` once at elaboration with a handle standing for every member: each event on it emits one record per
    * member, in this view's order, and nothing for an empty view. The body holds only events on the member. The loop
    * runs in the runtime: the site's sink sends one packet, and its handler emits a record for each member it finds.
    */
  def iter(body: T => Unit)(implicit sourceInfo: SourceInfo): Unit = EventTrace.iter(expr, gen(), sourceInfo)(body)

  /** The number of members, for a test in [[EventTrace.test]]: compare it with a constant, as in `view.size === 1`. */
  def size(implicit sourceInfo: SourceInfo): Size = new Size(expr, EventTrace.location(sourceInfo))

  private def choose(chooser: Chooser, sourceInfo: SourceInfo): T = {
    val handle = gen()
    handle.binding = Binding.Chosen(expr, chooser, EventTrace.location(sourceInfo))
    handle
  }
}

/** A view's number of members, which only a test in [[EventTrace.test]] can use: the runtime counts the members, so
  * the count is not known at elaboration. Comparing it with Scala's `==` would compile to a test that is always false,
  * so it stops elaboration instead.
  */
final class Size private[eventtrace] (private[eventtrace] val view: ViewExpr, location: String) {
  def ===(k: Int)(implicit sourceInfo: SourceInfo): Test = test(CompareOp.Eq, k, sourceInfo)
  def =/=(k: Int)(implicit sourceInfo: SourceInfo): Test = test(CompareOp.Ne, k, sourceInfo)
  def <(k: Int)(implicit sourceInfo: SourceInfo): Test = test(CompareOp.Lt, k, sourceInfo)
  def <=(k: Int)(implicit sourceInfo: SourceInfo): Test = test(CompareOp.Le, k, sourceInfo)
  def >(k: Int)(implicit sourceInfo: SourceInfo): Test = test(CompareOp.Gt, k, sourceInfo)
  def >=(k: Int)(implicit sourceInfo: SourceInfo): Test = test(CompareOp.Ge, k, sourceInfo)

  private def test(op: CompareOp, k: Int, sourceInfo: SourceInfo): Test = {
    if (k < 0) EventTrace.elaborationError(s"compares a view's size with $k; a size is at least 0", sourceInfo)
    new Test(view, op, k)
  }

  override def equals(that: Any): Boolean = throw new IllegalArgumentException(
    s"a view's size is compared with Scala's ==, which runs at elaboration, when the runtime has not counted the " +
      s"members; compare it with === inside EventTrace.test ($location)"
  )
  override def hashCode: Int = System.identityHashCode(this)
}

/** A comparison of a view's size with a constant, which the runtime evaluates; see [[EventTrace.test]]. */
final class Test private[eventtrace] (
  private[eventtrace] val view:  ViewExpr,
  private[eventtrace] val op:    CompareOp,
  private[eventtrace] val value: Int)

/** A test gating a site, with the answer under which the site's records are kept: true inside `EventTrace.test`,
  * false inside its `otherwise`.
  */
private[eventtrace] final case class Guard(test: Test, holds: Boolean)

/** A field or an object's `id`, which `orderBy` can rank by. Only this library implements it. */
trait Key {
  private[eventtrace] def operand: Operand
}

/** An object's ID: compare it with another object's in a query, or capture it as an event field. */
final class IdRef private[eventtrace] (private[eventtrace] val handle: ObjectType) extends Key {
  private[eventtrace] def operand: Operand = Operand.Id(handle)

  def ===(that: IdRef)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Eq, that)
  def =/=(that: IdRef)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Ne, that)
  def <(that: IdRef)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Lt, that)
  def <=(that: IdRef)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Le, that)
  def >(that: IdRef)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Gt, that)
  def >=(that: IdRef)(implicit sourceInfo: SourceInfo): Pred = compare(CompareOp.Ge, that)

  private def compare(op: CompareOp, that: IdRef)(implicit sourceInfo: SourceInfo): Pred = {
    if (that.handle.typeName != handle.typeName)
      EventTrace.elaborationError(
        s"compares an ID of type ${handle.typeName} with an ID of type ${that.handle.typeName}; IDs are counted per type",
        sourceInfo
      )
    Pred.Compare(op, operand, that.operand)
  }
}

/** A predicate in `where`, evaluated by the runtime. An unset field equals no value, as a floating-point NaN does:
  * `===` and the ordering comparisons are false when either side is unset, and `=/=` is true.
  */
sealed trait Pred {
  def &&(that: Pred): Pred = Pred.And(this, that)
  def ||(that: Pred): Pred = Pred.Or(this, that)
  def unary_! : Pred = Pred.Not(this)

  /** A hardware `Bool` as a term, as in `where(p => p.stage =/= Stage.Buffer || rvc)`; it travels in the payload. */
  def &&(that: Bool)(implicit sourceInfo: SourceInfo): Pred = Pred.And(this, Pred.holds(that, sourceInfo))
  def ||(that: Bool)(implicit sourceInfo: SourceInfo): Pred = Pred.Or(this, Pred.holds(that, sourceInfo))
}

private[eventtrace] object Pred {
  final case class Compare(op: CompareOp, left: Operand, right: Operand) extends Pred
  final case class And(left: Pred, right: Pred) extends Pred
  final case class Or(left: Pred, right: Pred) extends Pred
  final case class Not(pred: Pred) extends Pred

  /** A `Bool` as a predicate: it equals true. */
  def holds(b: Bool, sourceInfo: SourceInfo): Pred = {
    val bool = FieldType(1, signed = false, isBool = true, None)
    Compare(CompareOp.Eq, Queries.value(bool, b, "a Bool term", sourceInfo), Operand.Literal(1, bool))
  }
}

/** A comparison as written in diagnostics, and its C++ helper in `eventtrace::query`. */
private[eventtrace] sealed abstract class CompareOp(val written: String, val helper: String)

private[eventtrace] object CompareOp {
  case object Eq extends CompareOp("==", "eq")
  case object Ne extends CompareOp("!=", "ne")
  case object Lt extends CompareOp("<", "lt")
  case object Le extends CompareOp("<=", "le")
  case object Gt extends CompareOp(">", "gt")
  case object Ge extends CompareOp(">=", "ge")
}

/** A value in a query. */
private[eventtrace] sealed trait Operand {
  def tpe: FieldType
}

private[eventtrace] object Operand {

  /** A field of the member being tested or of another handle. */
  final case class Field(handle: ObjectType, field: ObjectField) extends Operand {
    def tpe: FieldType = field.tpe
  }

  /** The ID of the member being tested or of another handle. */
  final case class Id(handle: ObjectType) extends Operand {
    def tpe: FieldType = FieldType.id(handle.typeName)
  }

  /** A value known at elaboration, which the generated query contains. */
  final case class Literal(value: BigInt, tpe: FieldType) extends Operand

  /** A hardware value, which travels in the payload of each site using the query. It counts as a capture of those
    * sites, so it must be in their module. Compared by identity.
    */
  final class Hardware(val value: Data, val tpe: FieldType, val module: BaseModule, val location: String) extends Operand
}

/** How a query is built from a collection. Each `where` and `orderBy` has its own member handle, which stands for the
  * member being tested.
  */
private[eventtrace] sealed trait ViewExpr {
  def collection: CollectionInfo
}

private[eventtrace] object ViewExpr {
  final case class Source(collection: CollectionInfo) extends ViewExpr
  final case class Where(input: ViewExpr, member: ObjectType, pred: Pred) extends ViewExpr {
    def collection: CollectionInfo = input.collection
  }
  final case class OrderBy(input: ViewExpr, member: ObjectType, key: Operand) extends ViewExpr {
    def collection: CollectionInfo = input.collection
  }
}

/** How a handle picks one member of a view. */
private[eventtrace] sealed trait Chooser

private[eventtrace] object Chooser {
  case object Head extends Chooser
  case object Last extends Chooser
  final case class At(k: Int) extends Chooser
  case object Only extends Chooser
}

/** A collection's declaration: its object type and, for diagnostics, its module instance and `val` name. */
private[eventtrace] final class CollectionInfo(
  val index:    Int,
  val typeName: String,
  val module:   BaseModule,
  val valName:  String) {

  /** The declaring module's instance path and the `val` name, such as `Core.active`; valid once elaboration has
    * finished.
    */
  def name: String = s"${module.pathName}.$valName"
}

/** A state change made by an event, applied when the cycle's records have all been handled. */
private[eventtrace] sealed trait Effect

private[eventtrace] object Effect {
  final case class Add(collection: CollectionInfo) extends Effect
  final case class Remove(collection: CollectionInfo) extends Effect
  case object Destroy extends Effect
}

/** What produced a handle, which decides where it may be used. */
private[eventtrace] sealed trait Binding

private[eventtrace] object Binding {

  /** Constructed, but produced by no create block or query. */
  case object Unbound extends Binding

  /** The subject of a create block, valid only inside it. */
  final case class Created(open: Boolean) extends Binding

  /** Picked from a view by a chooser at `location`; valid anywhere. */
  final case class Chosen(view: ViewExpr, chooser: Chooser, location: String) extends Binding

  /** The member being tested by a `where` or `orderBy`, valid only inside its predicate or key. */
  final case class Member(open: Boolean) extends Binding

  /** Every member of a view, valid only inside its `iter` block. */
  final case class IterMember(view: ViewExpr, open: Boolean) extends Binding
}

private[eventtrace] object Queries {
  def member[T <: ObjectType](handle: T): T = {
    handle.binding = Binding.Member(open = true)
    handle
  }

  def close(handle: ObjectType): Unit = handle.binding = Binding.Member(open = false)

  /** A predicate may refer to its own member and to handles that can be resolved before it: not to a create
    * handle, whose object does not exist before the edge, nor to the member of another query.
    */
  def checkPred(pred: Pred, member: ObjectType, sourceInfo: SourceInfo): Unit = {
    def check(o: Operand): Unit = o match {
      case Operand.Field(h, _) => checkHandle(h)
      case Operand.Id(h)       => checkHandle(h)
      case _                   =>
    }
    def checkHandle(h: ObjectType): Unit = if (!(h eq member)) h.binding match {
      case _: Binding.Chosen | Binding.IterMember(_, true) =>
      case Binding.Member(_) =>
        EventTrace.elaborationError("a where refers to the member of another query; queries cannot be nested", sourceInfo)
      case Binding.Created(_) =>
        EventTrace.elaborationError(
          "a where refers to a create block's subject, which does not exist before the clock edge",
          sourceInfo
        )
      case Binding.IterMember(_, false) =>
        EventTrace.elaborationError("a where refers to an iter member after its iter block closed", sourceInfo)
      case Binding.Unbound =>
        EventTrace.elaborationError("a where refers to an object that no create block or query produced", sourceInfo)
    }
    def walk(p: Pred): Unit = p match {
      case Pred.Compare(_, l, r) => check(l); check(r)
      case Pred.And(l, r)        => walk(l); walk(r)
      case Pred.Or(l, r)         => walk(l); walk(r)
      case Pred.Not(q)           => walk(q)
    }
    walk(pred)
  }

  /** The value on the right of a comparison with a field of type `field`. A `UInt` or `Bool` value compares with a
    * `UInt` or `Bool` field, and an `SInt` with an `SInt`.
    */
  def value(field: FieldType, v: Data, what: String, sourceInfo: SourceInfo): Operand = {
    val t = EventTrace.valueType(what, v, field.enumDef.map(_ => field), sourceInfo)
    if (field.enumDef.isEmpty && t.signed != field.signed)
      EventTrace.elaborationError(
        s"$what compares a${if (t.signed) "n SInt" else " UInt"} value with a ${field.typeName} field",
        sourceInfo
      )
    v.litOption match {
      case Some(lit) => Operand.Literal(lit, t)
      case None =>
        val module = Module.currentModule.getOrElse(EventTrace.elaborationError(s"$what is outside a module", sourceInfo))
        new Operand.Hardware(v, t, module, EventTrace.location(sourceInfo))
    }
  }

  /** Every handle a view refers to, other than its own members. */
  def handles(view: ViewExpr): Seq[ObjectType] = view match {
    case ViewExpr.Source(_)              => Nil
    case ViewExpr.Where(in, member, p)   => handles(in) ++ predHandles(p).filterNot(_ eq member)
    case ViewExpr.OrderBy(in, _, _)      => handles(in)
  }

  /** Every hardware value a view compares with. */
  def hardware(view: ViewExpr): Seq[Operand.Hardware] = view match {
    case ViewExpr.Source(_)        => Nil
    case ViewExpr.Where(in, _, p)  => hardware(in) ++ predOperands(p).collect { case h: Operand.Hardware => h }
    case ViewExpr.OrderBy(in, _, _) => hardware(in)
  }

  private def predOperands(p: Pred): Seq[Operand] = p match {
    case Pred.Compare(_, l, r) => Seq(l, r)
    case Pred.And(l, r)        => predOperands(l) ++ predOperands(r)
    case Pred.Or(l, r)         => predOperands(l) ++ predOperands(r)
    case Pred.Not(q)           => predOperands(q)
  }

  private def predHandles(p: Pred): Seq[ObjectType] = predOperands(p).collect {
    case Operand.Field(h, _) => h
    case Operand.Id(h)       => h
  }
}
