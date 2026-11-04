"""Declarations shared by the manifest builder and the encoder."""

from __future__ import annotations

import inspect
import types
from dataclasses import dataclass
from enum import Enum, IntEnum
from functools import wraps
from typing import Annotated, Union, get_args, get_origin, get_type_hints


class Lifecycle(Enum):
    """What an event does to its subject's life; the manifest's `lifecycle` value."""

    CREATE = "create"
    COMPLETE = "complete"
    ABORT = "abort"


class Role(Enum):
    """What an Id field means beyond its value, as the manifest's `role` names it.

    Attached as metadata, as in `Annotated[Id[Instruction] | None, Role.BLOCKED_BY]`.
    """

    PARENT = "parent"
    BLOCKED_BY = "blocked_by"
    ABORTED_BY = "aborted_by"


@dataclass(frozen=True)
class UInt:
    """An unsigned integer field's width, 1 to 64 bits: `Annotated[int, UInt(w)]`."""

    width: int

    def __post_init__(self):
        """Rejects a width that is not an int from 1 to 64."""
        if type(self.width) is not int or not 1 <= self.width <= 64:
            raise ValueError("integer width must be between 1 and 64")


@dataclass(frozen=True)
class SInt(UInt):
    """A signed integer field's width, 1 to 64 bits; the trace zigzag-encodes it."""

    pass


U8 = Annotated[int, UInt(8)]
U16 = Annotated[int, UInt(16)]
U32 = Annotated[int, UInt(32)]
U64 = Annotated[int, UInt(64)]
I32 = Annotated[int, SInt(32)]
I64 = Annotated[int, SInt(64)]


class _Marker:
    """A field marker: `Marker[T]` is `T` with the marker class as its metadata."""

    @classmethod
    def __class_getitem__(cls, ty):
        """Returns `ty` annotated with this marker class."""
        return Annotated[ty, cls]


class Const(_Marker):
    """Marks a const object field: the create event carries it, read from the object."""

    pass


class SetOnce(_Marker):
    """Marks a setOnce object field: unset at first, then assigned at most once."""

    pass


class Mutable(_Marker):
    """Marks a mutable object field, which events may assign any number of times."""

    pass


class Identity(_Marker):
    """Marks the object field holding the object's ID: one per object type, unsigned."""

    pass


class Counter(_Marker):
    """Marks a counter event's value field.

    A counter event has no subject, and its other fields are the counter's keys.
    """

    pass


@dataclass(frozen=True)
class _Ref:
    """The metadata `Id[T]` attaches: the object type an Id field refers to."""

    target: type


class Id:
    """`Id[T]` is a field holding the ID of a T object, or the object itself."""

    @classmethod
    def __class_getitem__(cls, target):
        """Returns `int` annotated with a reference to `target`."""
        return Annotated[int, _Ref(target)]


@dataclass
class Field:
    """A declared field: its manifest entry, and what encoding a value of it needs."""

    name: str
    schema: dict
    enum: type[IntEnum] | None = None
    reference: type | None = None
    optional: bool = False
    counter: bool = False
    identity: bool = False

    def encode(self, value):
        """Checks `value` against the field and returns the integer the trace carries.

        An object becomes its ID, an enum its value, and a bool 0 or 1; a signed value
        is zigzag-encoded. None, for an optional field, stays None, meaning absent.
        Raises for a missing required value, a wrong type, or a value outside the width.
        """
        if value is None:
            if self.optional:
                return None
            raise ValueError(f"{self.name} is required")
        if self.reference is not None and isinstance(value, self.reference):
            value = value.trace_identity()
        if self.enum is not None:
            if type(value) is not self.enum:
                raise TypeError(f"{self.name} must be {self.enum.__name__}")
            value = value.value
        if self.schema["type"] == "Bool":
            if type(value) is not bool:
                raise TypeError(f"{self.name} must be bool")
            return int(value)
        if not isinstance(value, int) or isinstance(value, bool):
            raise TypeError(f"{self.name} must be an integer")
        width = self.schema.get("width", 64)
        signed = self.schema["type"] == "SInt"
        lower, upper = (
            (-(1 << (width - 1)), 1 << (width - 1)) if signed else (0, 1 << width)
        )
        if not lower <= value < upper:
            raise ValueError(f"{self.name}={value} is outside its {width}-bit range")
        return ((value << 1) ^ (value >> 63)) if signed else value


def field(name, annotation):
    """Builds the Field that a type annotation declares.

    Unwraps Annotated metadata and Optional, which makes the field optional, and takes
    the type from Id[T], bool, an IntEnum, or an int with a UInt or SInt width, and the
    property, role, counter, and identity from the markers. Raises TypeError for an
    annotation it cannot encode.
    """
    metadata = []
    optional = False
    while True:
        origin, args = get_origin(annotation), get_args(annotation)
        if origin is Annotated:
            annotation = args[0]
            metadata.extend(args[1:])
        elif origin in (Union, types.UnionType) and type(None) in args:
            choices = [a for a in args if a is not type(None)]
            if len(choices) != 1:
                raise TypeError(f"{name}: optional fields must have one value type")
            annotation, optional = choices[0], True
        else:
            break
    schema = {"name": name}
    enum = reference = None
    number = next((m for m in metadata if isinstance(m, UInt)), None)
    ref = next((m for m in metadata if isinstance(m, _Ref)), None)
    if ref:
        reference = ref.target
        schema.update(type="Id", object=reference.__eventtrace_name__)
    elif annotation is bool:
        schema.update(type="Bool", width=1)
    elif inspect.isclass(annotation) and issubclass(annotation, IntEnum):
        enum = annotation
        if any(not 0 <= v.value < 1 << 64 for v in enum):
            raise ValueError(f"{enum.__name__} must contain unsigned 64-bit values")
        schema.update(
            type="Enum",
            enum=enum.__name__,
            width=max(1, max((v.value.bit_length() for v in enum), default=1)),
        )
    elif annotation is int and number:
        schema.update(
            type="SInt" if isinstance(number, SInt) else "UInt", width=number.width
        )
    else:
        raise TypeError(
            f"{name}: use an explicit integer width, bool, IntEnum, or Id[T]"
        )
    for marker, property_ in (
        (Const, "const"),
        (SetOnce, "setOnce"),
        (Mutable, "mutable"),
    ):
        if marker in metadata:
            schema["property"] = property_
    role = next((m for m in metadata if isinstance(m, Role)), None)
    if role:
        if not ref:
            raise TypeError(f"{name}: a relationship role requires Id[T]")
        schema["role"] = role.value
    if optional:
        schema["optional"] = True
    return Field(
        name,
        schema,
        enum,
        reference,
        optional,
        Counter in metadata,
        Identity in metadata,
    )


def object_fields(cls):
    """An object class's fields, from its annotations and its properties' return types.

    Keeps the identity field and the fields with a property marker.
    """
    hints = get_type_hints(cls, localns={cls.__name__: cls}, include_extras=True)
    for name, member in inspect.getmembers(cls):
        if isinstance(member, property):
            annotation = get_type_hints(
                member.fget, localns={cls.__name__: cls}, include_extras=True
            ).get("return")
            if annotation is not None:
                hints[name] = annotation
    fields = []
    for name, annotation in hints.items():
        if get_origin(annotation) is Annotated:
            f = field(name, annotation)
            if f.identity or "property" in f.schema:
                fields.append(f)
    return fields


class TraceSource:
    """A model component connected to a shared writer; no simulation state."""

    def bind_trace(self, writer, *, scope=""):
        """Sends this source's events to `writer`, under `scope`; returns self.

        The writer must have the source's class registered with `scope`. Until bound,
        the source's event methods record nothing.
        """
        self._eventtrace_writer = writer
        self._eventtrace_scope = scope
        return self


class TracedObject(TraceSource):
    """A source that is an object: its events' subject, named by its Identity field."""

    def trace_identity(self):
        """The object's ID, its Identity field's value as the trace carries it."""
        f = type(self).__eventtrace_identity__
        return f.encode(getattr(self, f.name))


def object_type(*, name):
    """Class decorator declaring a TracedObject subclass as the object type `name`."""

    def decorate(cls):
        """Records `cls`'s name and fields, checking it has one unsigned identity."""
        if not issubclass(cls, TracedObject):
            raise TypeError("traced objects must inherit TracedObject")
        cls.__eventtrace_name__ = name
        fields = object_fields(cls)
        ids = [f for f in fields if f.identity]
        if len(ids) != 1 or ids[0].schema["type"] != "UInt":
            raise TypeError(f"{name}: declare exactly one unsigned Identity field")
        cls.__eventtrace_identity__ = ids[0]
        cls.__eventtrace_fields__ = [f for f in fields if not f.identity]
        return cls

    return decorate


@dataclass(eq=False)
class Event:
    """An event method's declaration, which `event` attaches to its recorder.

    `blocks` says whether its site finds the subject blocked, and `constants` maps
    the object fields it assigns to their values.
    """

    function: object
    lifecycle: Lifecycle | None
    blocks: bool
    constants: dict

    def __post_init__(self):
        """Reads the signature, which must start (self, cycle) and name every field."""
        self.signature = inspect.signature(self.function)
        parameters = list(self.signature.parameters.values())
        if len(parameters) < 2 or [p.name for p in parameters[:2]] != ["self", "cycle"]:
            raise TypeError("event methods start with (self, cycle, ...)")
        if any(p.kind in (p.VAR_POSITIONAL, p.VAR_KEYWORD) for p in parameters):
            raise TypeError("event methods must declare each field explicitly")
        if self.lifecycle is not None and not isinstance(self.lifecycle, Lifecycle):
            raise TypeError("lifecycle must be a Lifecycle value")

    def fields(self, cls):
        """The event's fields on source class `cls`, sorted by name.

        They are its parameters after cycle, its constants, and, for a create event,
        every const field of the object. Raises TypeError for a field that disagrees
        with the object's declaration of it.
        """
        hints = get_type_hints(self.function, include_extras=True)
        result = {
            name: field(name, hints[name])
            for name in self.signature.parameters
            if name not in ("self", "cycle")
        }
        declared = {f.name: f for f in getattr(cls, "__eventtrace_fields__", [])}
        for name in self.constants:
            if name not in declared:
                raise TypeError(
                    f"{name}: event constants must name a declared object field"
                )
            if name in result:
                raise TypeError(f"{name}: declared as both a parameter and a constant")
            result[name] = declared[name]
        if self.lifecycle is Lifecycle.CREATE:
            for f in declared.values():
                if f.schema.get("property") == "const":
                    if f.name in result:
                        raise TypeError(
                            f"{f.name}: const fields are extracted from the object"
                        )
                    result[f.name] = f
        for f in result.values():
            if f.name in declared:
                expected = {
                    k: v for k, v in declared[f.name].schema.items() if k != "property"
                }
                actual = {k: v for k, v in f.schema.items() if k != "property"}
                if expected != actual:
                    raise TypeError(
                        f"{f.name}: event field disagrees with its object field"
                    )
        return sorted(result.values(), key=lambda f: f.name)


def event(function=None, *, lifecycle=None, blocks=False, **constants):
    """Implement an event method from its declaration; the body is not executed."""

    def decorate(fn):
        """Returns a recorder for `fn`, with its Event as `__eventtrace_event__`."""
        descriptor = Event(fn, lifecycle, blocks, constants)

        @wraps(fn)
        def emit(self, *args, **kwargs):
            """Records the call, with the constants, as one event at `cycle`.

            Does nothing while the source is unbound.
            """
            writer = getattr(self, "_eventtrace_writer", None)
            if writer is None:
                return
            bound = descriptor.signature.bind(self, *args, **kwargs)
            bound.apply_defaults()
            values = dict(bound.arguments)
            del values["self"]
            cycle = values.pop("cycle")
            values.update(descriptor.constants)
            writer._emit(self, descriptor, cycle, values)

        emit.__eventtrace_event__ = descriptor
        return emit

    return decorate(function) if function is not None else decorate
