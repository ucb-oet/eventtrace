// oet: EventTrace for designs that call into C++ at each traced point, such as a SystemVerilog design through DPI.
//
// The design's C++ declares object types, enums, collections, events, and calls, and each DPI function queues a
// handler with oet::at. The handler queries collections and emits events with Chisel's semantics: queries read the
// state from before the clock edge, and an event's effects hold from the next cycle. The library writes the trace
// through the shared runtime and builds the manifest from the declarations. See docs/plans/systemverilog.md.
#pragma once

#if __cplusplus < 202002L
#error "oet.h needs C++20"
#endif

#include <algorithm>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "eventtrace_runtime.h"
#include "eventtrace_tracker.h"

// An event's values are given with designated initializers that leave out the ones a record does not carry, which is
// the intended use; GCC's -Wextra warns about each, so the warning is off in the file that includes this header.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

// OET_DETAIL_FOR_EACH(m, d, a, b, ...) expands to m(d, a), m(d, b), ...: up to 64 arguments, the most fields an
// object type or event may have.
#define OET_DETAIL_EXPAND(x) x
#define OET_DETAIL_FE_1(m, d, x) m(d, x)
#define OET_DETAIL_FE_2(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_1(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_3(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_2(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_4(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_3(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_5(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_4(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_6(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_5(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_7(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_6(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_8(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_7(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_9(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_8(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_10(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_9(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_11(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_10(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_12(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_11(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_13(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_12(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_14(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_13(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_15(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_14(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_16(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_15(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_17(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_16(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_18(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_17(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_19(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_18(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_20(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_19(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_21(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_20(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_22(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_21(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_23(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_22(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_24(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_23(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_25(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_24(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_26(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_25(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_27(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_26(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_28(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_27(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_29(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_28(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_30(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_29(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_31(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_30(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_32(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_31(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_33(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_32(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_34(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_33(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_35(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_34(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_36(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_35(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_37(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_36(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_38(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_37(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_39(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_38(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_40(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_39(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_41(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_40(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_42(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_41(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_43(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_42(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_44(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_43(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_45(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_44(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_46(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_45(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_47(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_46(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_48(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_47(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_49(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_48(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_50(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_49(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_51(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_50(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_52(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_51(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_53(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_52(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_54(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_53(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_55(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_54(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_56(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_55(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_57(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_56(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_58(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_57(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_59(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_58(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_60(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_59(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_61(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_60(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_62(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_61(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_63(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_62(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_64(m, d, x, ...) m(d, x), OET_DETAIL_EXPAND(OET_DETAIL_FE_63(m, d, __VA_ARGS__))
#define OET_DETAIL_FE_PICK(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, _33, _34, _35, _36, _37, _38, _39, _40, _41, _42, _43, _44, _45, _46, _47, _48, _49, _50, _51, _52, _53, _54, _55, _56, _57, _58, _59, _60, _61, _62, _63, _64, NAME, ...) NAME
#define OET_DETAIL_FOR_EACH(m, d, ...) \
  OET_DETAIL_EXPAND(OET_DETAIL_FE_PICK(__VA_ARGS__, OET_DETAIL_FE_64, OET_DETAIL_FE_63, OET_DETAIL_FE_62, OET_DETAIL_FE_61, OET_DETAIL_FE_60, OET_DETAIL_FE_59, OET_DETAIL_FE_58, OET_DETAIL_FE_57, OET_DETAIL_FE_56, OET_DETAIL_FE_55, OET_DETAIL_FE_54, OET_DETAIL_FE_53, OET_DETAIL_FE_52, OET_DETAIL_FE_51, OET_DETAIL_FE_50, OET_DETAIL_FE_49, OET_DETAIL_FE_48, OET_DETAIL_FE_47, OET_DETAIL_FE_46, OET_DETAIL_FE_45, OET_DETAIL_FE_44, OET_DETAIL_FE_43, OET_DETAIL_FE_42, OET_DETAIL_FE_41, OET_DETAIL_FE_40, OET_DETAIL_FE_39, OET_DETAIL_FE_38, OET_DETAIL_FE_37, OET_DETAIL_FE_36, OET_DETAIL_FE_35, OET_DETAIL_FE_34, OET_DETAIL_FE_33, OET_DETAIL_FE_32, OET_DETAIL_FE_31, OET_DETAIL_FE_30, OET_DETAIL_FE_29, OET_DETAIL_FE_28, OET_DETAIL_FE_27, OET_DETAIL_FE_26, OET_DETAIL_FE_25, OET_DETAIL_FE_24, OET_DETAIL_FE_23, OET_DETAIL_FE_22, OET_DETAIL_FE_21, OET_DETAIL_FE_20, OET_DETAIL_FE_19, OET_DETAIL_FE_18, OET_DETAIL_FE_17, OET_DETAIL_FE_16, OET_DETAIL_FE_15, OET_DETAIL_FE_14, OET_DETAIL_FE_13, OET_DETAIL_FE_12, OET_DETAIL_FE_11, OET_DETAIL_FE_10, OET_DETAIL_FE_9, OET_DETAIL_FE_8, OET_DETAIL_FE_7, OET_DETAIL_FE_6, OET_DETAIL_FE_5, OET_DETAIL_FE_4, OET_DETAIL_FE_3, OET_DETAIL_FE_2, OET_DETAIL_FE_1)(m, d, __VA_ARGS__))

namespace oet {

struct Object;
struct none {};  // the subject of an event without one, or the values of an event with none of its own
template <class T>
class Id;

namespace detail {

enum class Kind { Bool, UInt, SInt, Enum, Id };
enum class Property { Const, SetOnce, Mutable };
enum class Lifecycle { None, Create, Complete, Abort };

// An enum's name and its variants' names and values, in declaration order.
struct EnumDecl {
  std::string name;
  std::vector<std::pair<std::string, uint64_t>> variants;
  unsigned width = 1;  // enough for the largest value, as a ChiselEnum's
};
EnumDecl make_enum(const char* name, const char* names, std::initializer_list<uint64_t> values);

struct ObjectType;

// A value's type as the manifest gives it.
struct Type {
  Kind kind;
  unsigned width;                    // 0 for an Id
  const EnumDecl* (*enum_decl)();    // an Enum's
  const ObjectType& (*object)();     // an Id's object type
};

// One value as a record holds it: whether it was set, its bits (two's complement for an SInt), and whether they fit
// its type.
struct Encoded {
  bool set = false;
  uint64_t bits = 0;
  bool fits = true;
};

template <class I>
constexpr bool negative(I v) {
  if constexpr (std::is_signed_v<I>) return v < 0;
  else return false;
}

// Stops the run, naming the running handler's call and cycle, if any.
[[noreturn]] void fail(const std::string& message);

template <class T>
const ObjectType& object_type();

struct FieldAccess;

}  // namespace detail

// Values. Each knows whether it was set. One that was not compares false with everything, as an unset field does in
// Chisel's queries, and != is true.

class Bool {
 public:
  constexpr Bool() = default;
  constexpr Bool(bool v) : v_(v), set_(true) {}
  constexpr bool is_set() const { return set_; }
  constexpr bool value() const { return v_; }
  friend constexpr bool operator==(const Bool& a, const Bool& b) { return a.set_ && b.set_ && a.v_ == b.v_; }
  friend constexpr std::partial_ordering operator<=>(const Bool& a, const Bool& b) {
    if (!a.set_ || !b.set_) return std::partial_ordering::unordered;
    return a.v_ <=> b.v_;
  }
  static detail::Type type() { return {detail::Kind::Bool, 1, nullptr, nullptr}; }
  detail::Encoded encoded() const { return {set_, v_, true}; }

 private:
  bool v_ = false, set_ = false;
};

template <unsigned N>
class UInt {
  static_assert(N >= 1 && N <= 64, "a value is 1 to 64 bits wide");

 public:
  constexpr UInt() = default;
  template <std::integral I>
  constexpr UInt(I v) : v_(static_cast<uint64_t>(v)), set_(true), negative_(detail::negative(v)) {}
  constexpr bool is_set() const { return set_; }
  constexpr uint64_t value() const { return v_; }
  friend constexpr bool operator==(const UInt& a, const UInt& b) { return a.set_ && b.set_ && a.v_ == b.v_; }
  friend constexpr std::partial_ordering operator<=>(const UInt& a, const UInt& b) {
    if (!a.set_ || !b.set_) return std::partial_ordering::unordered;
    return a.v_ <=> b.v_;
  }
  static detail::Type type() { return {detail::Kind::UInt, N, nullptr, nullptr}; }
  detail::Encoded encoded() const { return {set_, v_, !negative_ && (N == 64 || v_ >> N == 0)}; }

 private:
  uint64_t v_ = 0;
  bool set_ = false, negative_ = false;
};

template <unsigned N>
class SInt {
  static_assert(N >= 1 && N <= 64, "a value is 1 to 64 bits wide");

 public:
  constexpr SInt() = default;
  template <std::integral I>
  constexpr SInt(I v)
      : v_(static_cast<int64_t>(v)), set_(true),
        huge_(!std::is_signed_v<I> && static_cast<uint64_t>(v) > static_cast<uint64_t>(INT64_MAX)) {}
  constexpr bool is_set() const { return set_; }
  constexpr int64_t value() const { return v_; }
  friend constexpr bool operator==(const SInt& a, const SInt& b) { return a.set_ && b.set_ && a.v_ == b.v_; }
  friend constexpr std::partial_ordering operator<=>(const SInt& a, const SInt& b) {
    if (!a.set_ || !b.set_) return std::partial_ordering::unordered;
    return a.v_ <=> b.v_;
  }
  static detail::Type type() { return {detail::Kind::SInt, N, nullptr, nullptr}; }
  detail::Encoded encoded() const {
    const bool fits = !huge_ && (N == 64 || (v_ >= -(int64_t{1} << (N - 1)) && v_ < (int64_t{1} << (N - 1))));
    return {set_, static_cast<uint64_t>(v_), fits};
  }

 private:
  int64_t v_ = 0;
  bool set_ = false, huge_ = false;
};

// The ID of an object of type T: a field or value naming another object.
template <class T>
class Id {
 public:
  constexpr Id() = default;
  Id(const T& o) : v_(o.id), set_(true) {}
  constexpr Id(unsigned long long id) : v_(id), set_(true) {}
  constexpr bool is_set() const { return set_; }
  constexpr unsigned long long value() const { return v_; }
  friend constexpr bool operator==(const Id& a, const Id& b) { return a.set_ && b.set_ && a.v_ == b.v_; }
  friend constexpr std::partial_ordering operator<=>(const Id& a, const Id& b) {
    if (!a.set_ || !b.set_) return std::partial_ordering::unordered;
    return a.v_ <=> b.v_;
  }
  static detail::Type type() { return {detail::Kind::Id, 0, nullptr, &detail::object_type<T>}; }
  detail::Encoded encoded() const { return {set_, v_, true}; }

 private:
  unsigned long long v_ = 0;
  bool set_ = false;
};

namespace detail {

template <class V>
struct ValueOf {
  using type = V;
};
template <>
struct ValueOf<bool> {
  using type = Bool;
};

template <class V>
concept Ordered = requires(const V& a) { a <=> a; };

template <class T>
bool is_set(const T& v) {
  return v.is_set();
}

}  // namespace detail

// An object type's field, which reads as its value from before the clock edge. Assigning it inside a handler stages
// the write for the next event emitted on the object, which carries the value and applies it from the next cycle.
template <class V, detail::Property P, bool IsParent = false>
class Field {
 public:
  using value_type = V;
  static constexpr detail::Property property = P;
  static constexpr bool is_parent = IsParent;

  Field() = default;
  Field(const Field&) = delete;
  Field& operator=(const V& v);
  Field& operator=(const Field& other) { return *this = other.value_; }

  const V& get() const { return value_; }
  operator const V&() const { return value_; }
  bool is_set() const { return value_.is_set(); }

  friend bool operator==(const Field& a, const V& b) { return a.value_ == b; }
  friend std::partial_ordering operator<=>(const Field& a, const V& b)
    requires detail::Ordered<V>
  {
    return a.value_ <=> b;
  }

 private:
  friend struct detail::FieldAccess;
  V value_{};
  Object* owner_ = nullptr;  // set when the library creates the object
  unsigned index_ = 0;
};

template <class V>
using Const = Field<typename detail::ValueOf<V>::type, detail::Property::Const>;
template <class V>
using SetOnce = Field<typename detail::ValueOf<V>::type, detail::Property::SetOnce>;
template <class V>
using Mutable = Field<typename detail::ValueOf<V>::type, detail::Property::Mutable>;
// The const field naming the object one came from; a type has at most one.
template <class T>
using Parent = Field<Id<T>, detail::Property::Const, true>;

namespace detail {

class Library;
class EventBase;

// A field's declaration in an object type.
struct FieldDecl {
  std::string name;
  Property property;
  bool parent;
  Type type;
};

struct ObjectType {
  std::string name;
  unsigned index = 0;  // for the runtime's per-type ID counters
  std::vector<FieldDecl> fields;
  void (*bind)(Object&) = nullptr;  // tells each field of a new object its owner and index
};

const ObjectType& register_object(ObjectType type);

struct FieldAccess {
  template <class F>
  static void bind(F& f, Object& owner, unsigned index) {
    f.owner_ = &owner;
    f.index_ = index;
  }
  template <class F>
  static const auto& value(const F& f) {
    return f.value_;
  }
};

std::vector<std::string> split_names(const char* names);

template <class... Ms>
struct ObjectDecl {
  const char* name;
  const char* names;
  std::tuple<Ms...> members;
};
template <class... Ms>
ObjectDecl<Ms...> object_decl(const char* name, const char* names, Ms... members) {
  return {name, names, std::tuple<Ms...>(members...)};
}

template <class M>
struct MemberField;
template <class C, class V, Property P, bool IsParent>
struct MemberField<Field<V, P, IsParent> C::*> {
  using value_type = V;
  static constexpr Property property = P;
  static constexpr bool is_parent = IsParent;
};

template <class T>
const ObjectType& object_type() {
  static const ObjectType* const type = []() -> const ObjectType* {
    const auto decl = oet_object_decl(static_cast<T*>(nullptr));
    const std::vector<std::string> names = split_names(decl.names);
    ObjectType t;
    t.name = decl.name;
    std::apply(
        [&](auto... members) {
          unsigned i = 0;
          ((t.fields.push_back({names.at(i++), MemberField<decltype(members)>::property,
                                MemberField<decltype(members)>::is_parent,
                                MemberField<decltype(members)>::value_type::type()})),
           ...);
        },
        decl.members);
    t.bind = [](Object& o) {
      T& self = static_cast<T&>(o);
      std::apply(
          [&](auto... members) {
            unsigned i = 0;
            (FieldAccess::bind(self.*members, o, i++), ...);
          },
          oet_object_decl(static_cast<T*>(nullptr)).members);
    };
    return &register_object(std::move(t));
  }();
  return *type;
}

// The index of `member` among T's fields as OET_OBJECT lists them, or -1.
template <class T, class M>
int field_index(M T::*member) {
  int found = -1, i = 0;
  std::apply(
      [&](auto... members) {
        (
            [&](auto m) {
              if constexpr (std::is_same_v<decltype(m), M T::*>)
                if (m == member && found < 0) found = i;
              ++i;
            }(members),
            ...);
      },
      oet_object_decl(static_cast<T*>(nullptr)).members);
  return found;
}

// The number of members of an aggregate, by the largest number of initializers it takes.
struct AnyMember {
  template <class U>
  constexpr operator U&() const noexcept;
};
template <class S, std::size_t... I>
constexpr bool takes_initializers(std::index_sequence<I...>) {
  return requires { S{(void(I), AnyMember{})...}; };
}
template <class S, std::size_t N = 0>
constexpr std::size_t member_count() {
  if constexpr (takes_initializers<S>(std::make_index_sequence<N + 1>{})) return member_count<S, N + 1>();
  else return N;
}

template <class M>
struct ValueMember {
  using value_type = M;
  static constexpr bool optional = false;
  static Encoded read(const M& v) { return v.encoded(); }
};
template <class V>
struct ValueMember<std::optional<V>> {
  using value_type = V;
  static constexpr bool optional = true;
  static Encoded read(const std::optional<V>& v) { return v ? v->encoded() : Encoded{}; }
};

// One value of an event beyond its subject's fields: a member of the struct OET_FIELDS registers.
struct ValueDecl {
  std::string name;
  Type type;
  bool optional;
  std::function<Encoded(const void*)> read;
};

template <class S, class... Ms>
std::vector<ValueDecl> fields_decl(const char* names, Ms... members) {
  static_assert(sizeof...(Ms) == member_count<S>(), "OET_FIELDS must list every member of the struct");
  const std::vector<std::string> split = split_names(names);
  std::vector<ValueDecl> out;
  unsigned i = 0;
  (
      [&](auto m) {
        using Member = std::remove_cvref_t<decltype(static_cast<const S*>(nullptr)->*m)>;
        out.push_back({split.at(i++), ValueMember<Member>::value_type::type(), ValueMember<Member>::optional,
                       [m](const void* s) { return ValueMember<Member>::read(static_cast<const S*>(s)->*m); }});
      }(members),
      ...);
  return out;
}

// Stores and fetches an object type's live objects.
template <class T>
eventtrace::Store<T>& store() {
  static eventtrace::Store<T> s;
  return s;
}

struct CreateTag {};
struct CompleteTag {};
struct AbortTag {};
template <class T>
struct ByTag {};
template <class F>
struct OptionalField {
  F member;
};

// How a record is emitted: plain, blocked (naming its cause or not), or naming the object that aborted its subject.
struct Mode {
  enum class Kind { Plain, Blocked, AbortedBy } kind = Kind::Plain;
  std::optional<unsigned long long> cause;
  const ObjectType& (*cause_type)() = nullptr;
};

// What a pending membership change does once its event is emitted.
using Move = std::function<void(unsigned site, unsigned long long event)>;

}  // namespace detail

// The base of every object type. Its members besides the eventtrace::Object ones are the library's bookkeeping.
struct Object : eventtrace::Object {
  struct PendingAssign {
    unsigned index;
    detail::Encoded value;
    bool set_once_taken;
    std::function<void()> write;
  };
  const detail::ObjectType* oet_type = nullptr;
  std::vector<PendingAssign> oet_assigns;  // field writes waiting for an event on the object
  std::vector<detail::Move> oet_moves;     // collection changes waiting likewise
  long long oet_created = -1;              // the cycle a create event made the object in
  const detail::EventBase* oet_created_by = nullptr;
  bool oet_create_emitted = false;
};

// The type name in the shared tracker's messages.
inline const char* type_name(const Object& o) { return o.oet_type ? o.oet_type->name.c_str() : "object"; }

inline constexpr detail::CreateTag create;
inline constexpr detail::CompleteTag complete;
inline constexpr detail::AbortTag abort;

// Declares the type of the object a record may name as what blocks or aborted its subject: oet::by<T>().
template <class T>
constexpr detail::ByTag<T> by() {
  return {};
}
// Names the object that aborted the subject, at emit.
template <class T>
  requires std::derived_from<T, Object>
detail::Mode by(const T& cause) {
  return {detail::Mode::Kind::AbortedBy, cause.id, &detail::object_type<T>};
}
template <class T>
detail::Mode by(const Id<T>& cause) {
  return {detail::Mode::Kind::AbortedBy, cause.value(), &detail::object_type<T>};
}

// The record finds its subject blocked, by an unknown cause or by `cause`.
inline detail::Mode blocked() { return {detail::Mode::Kind::Blocked, std::nullopt, nullptr}; }
template <class T>
  requires std::derived_from<T, Object>
detail::Mode blocked(const T& cause) {
  return {detail::Mode::Kind::Blocked, cause.id, &detail::object_type<T>};
}
template <class T>
detail::Mode blocked(const Id<T>& cause) {
  return {detail::Mode::Kind::Blocked, cause.value(), &detail::object_type<T>};
}

// Lists a subject field that some records of an event leave unassigned.
template <class M, class T>
detail::OptionalField<M T::*> optional(M T::*member) {
  return {member};
}

namespace detail {

// An event, without its types: what the manifest, the sites, and emit's checks need.
class EventBase {
 public:
  explicit EventBase(const char* name);
  EventBase(const EventBase&) = delete;
  EventBase& operator=(const EventBase&) = delete;

  std::string full_name() const;  // the manifest's name, qualified by the subject's type

  std::string name;
  const ObjectType& (*subject)() = nullptr;
  Lifecycle lifecycle = Lifecycle::None;
  const ObjectType& (*cause)() = nullptr;  // from oet::by<T>()
  struct Listed {
    int index;  // among the subject's fields
    bool optional;
  };
  std::vector<Listed> listed;
  std::vector<ValueDecl> values;
  bool has_values = false;

  // The manifest's fields, sorted by name, and where each record's value of each comes from.
  struct ManifestField {
    std::string name;
    Type type;
    bool optional;
    const char* role;  // or null
    enum class Source { Subject, Value, Cause } source;
    unsigned index;  // the subject field's or the value's
  };
  std::vector<ManifestField> fields;

 protected:
  // Checks and writes one record, then stages its effects; `destroy` stages the end of the subject.
  void emit_record(Object* subject, const void* values, const Mode& mode,
                   const std::function<void(unsigned site, unsigned long long event)>& destroy);
  Object& create_object(const ObjectType& type, Object& (*make)(unsigned long long id));
};

}  // namespace detail

// An event: its subject's type T (or oet::none), and the struct of the values it carries beyond T's fields (or
// oet::none). The declaration lists its lifecycle, the subject fields its records assign, and oet::by<C>().
template <class T = none, class Values = none>
class Event : public detail::EventBase {
  static constexpr bool kSubject = !std::is_same_v<T, none>;
  static constexpr bool kValues = !std::is_same_v<Values, none>;

 public:
  template <class... Options>
  explicit Event(const char* name, Options... options) : EventBase(name) {
    if constexpr (kSubject) subject = &detail::object_type<T>;
    if constexpr (kValues) {
      values = oet_fields_decl(static_cast<Values*>(nullptr));
      has_values = true;
    }
    (option(options), ...);
  }

  // Creates the subject of a create event, whose fields the handler then assigns before emitting the event on it.
  T& create()
    requires kSubject
  {
    return static_cast<T&>(create_object(detail::object_type<T>(), [](unsigned long long id) -> Object& {
      return detail::store<T>().create(id);
    }));
  }

  void emit(T& subject)
    requires kSubject
  {
    emit_on(subject, nullptr, {});
  }
  void emit(T& subject, const detail::Mode& mode)
    requires kSubject
  {
    emit_on(subject, nullptr, mode);
  }
  void emit(T& subject, const Values& v)
    requires(kSubject && kValues)
  {
    emit_on(subject, &v, {});
  }
  void emit(T& subject, const Values& v, const detail::Mode& mode)
    requires(kSubject && kValues)
  {
    emit_on(subject, &v, mode);
  }
  void emit()
    requires(!kSubject)
  {
    emit_record(nullptr, kValues ? &kNoValues : nullptr, {}, nullptr);
  }
  void emit(const Values& v)
    requires(!kSubject && kValues)
  {
    emit_record(nullptr, &v, {}, nullptr);
  }

 private:
  void option(detail::CreateTag) { lifecycle = detail::Lifecycle::Create; }
  void option(detail::CompleteTag) { lifecycle = detail::Lifecycle::Complete; }
  void option(detail::AbortTag) { lifecycle = detail::Lifecycle::Abort; }
  template <class C>
  void option(detail::ByTag<C>) {
    cause = &detail::object_type<C>;
  }
  template <class M>
  void option(M T::*member)
    requires kSubject
  {
    listed.push_back({detail::field_index(member), false});
  }
  template <class M>
  void option(detail::OptionalField<M T::*> f)
    requires kSubject
  {
    listed.push_back({detail::field_index(f.member), true});
  }

  void emit_on(T& subject, const Values* v, const detail::Mode& mode) {
    const void* values_ptr = nullptr;
    if constexpr (kValues) values_ptr = v ? static_cast<const void*>(v) : static_cast<const void*>(&kNoValues);
    emit_record(&subject, values_ptr, mode, [&subject](unsigned site, unsigned long long event) {
      destroy_subject(site, event, subject);
    });
  }
  static void destroy_subject(unsigned site, unsigned long long event, T& subject);

  static inline const Values kNoValues{};
};

class Call;

// A call and one of its indices: what oet::at queues a handler for.
struct CallSite {
  const Call* call;
  unsigned index;
  bool indexed;
};

// One DPI function's handler: a name for messages, how many indices it takes (a channel's slots, or a module's
// instances), and the events it emits. Each (call, index, event) has its own sites, numbered in declaration order.
class Call {
 public:
  using Events = std::initializer_list<std::reference_wrapper<detail::EventBase>>;
  Call(const char* name, Events events);
  Call(const char* name, unsigned count, Events events);
  Call(const Call&) = delete;
  Call& operator=(const Call&) = delete;

  CallSite operator[](unsigned index) const { return {this, index, true}; }

  std::string name;
  unsigned count;
  std::vector<detail::EventBase*> events;

  // Set when the trace opens: the first handler order of its indices, and each index's sites per event.
  unsigned first_slot = 0;
  std::vector<std::vector<unsigned>> sites;  // [index][event]: the non-blocking site; the blocking one follows it
};

namespace detail {

bool recording();
void queue(long long cycle, CallSite where, std::function<void()> handler);
void touch(Object& o);
long long current_cycle();
std::string describe(const Object& o);

template <class T>
eventtrace::Collection<T> make_collection(const char* name);

}  // namespace detail

// Queues `handler` for `cycle`: it runs once the cycle is complete, with the other handlers of the cycle in the
// order their calls were declared, and reads the state from before the clock edge. Without +eventtrace, it does
// nothing.
template <class F>
void at(long long cycle, CallSite where, F&& handler) {
  if (!detail::recording()) return;
  detail::queue(cycle, where, std::function<void()>(std::forward<F>(handler)));
}
template <class F>
void at(long long cycle, const Call& call, F&& handler) {
  at(cycle, CallSite{&call, 0, false}, std::forward<F>(handler));
}

// Members of a collection, as a query has narrowed or ordered them, in order.
template <class T>
class View {
 public:
  View(std::vector<T*> members, std::string what) : members_(std::move(members)), what_(std::move(what)) {}

  template <class P>
  View where(P&& pred) const {
    std::vector<T*> out;
    for (T* m : members_)
      if (pred(*m)) out.push_back(m);
    return View(std::move(out), what_ + ".where(...)");
  }
  // Ranks the members by `key`, ascending; ties keep their order. A member whose key is unset stops the run.
  template <class K>
  View order_by(K&& key) const {
    std::vector<T*> out = members_;
    for (T* m : out)
      if (!detail::is_set(key(*m))) detail::fail(detail::describe(*m) + " has no key to rank by in " + what_);
    std::stable_sort(out.begin(), out.end(), [&](T* a, T* b) { return key(*a) < key(*b); });
    return View(std::move(out), what_ + ".order_by(...)");
  }

  T& only() const {
    if (members_.size() != 1)
      detail::fail("only found " + std::to_string(members_.size()) + " members of " + what_ + names());
    return *members_.front();
  }
  T& head() const {
    if (members_.empty()) detail::fail("head found no members of " + what_);
    return *members_.front();
  }
  T& last() const {
    if (members_.empty()) detail::fail("last found no members of " + what_);
    return *members_.back();
  }
  T& at(std::size_t k) const {
    if (k >= members_.size())
      detail::fail("at(" + std::to_string(k) + ") found only " + std::to_string(members_.size()) + " members of " +
                   what_ + names());
    return *members_[k];
  }
  std::size_t size() const { return members_.size(); }
  bool empty() const { return members_.empty(); }

  class iterator {
   public:
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    iterator() = default;
    explicit iterator(typename std::vector<T*>::const_iterator i) : i_(i) {}
    T& operator*() const { return **i_; }
    T* operator->() const { return *i_; }
    iterator& operator++() {
      ++i_;
      return *this;
    }
    iterator operator++(int) {
      iterator old = *this;
      ++i_;
      return old;
    }
    bool operator==(const iterator&) const = default;

   private:
    typename std::vector<T*>::const_iterator i_;
  };
  iterator begin() const { return iterator(members_.begin()); }
  iterator end() const { return iterator(members_.end()); }

 private:
  // The members' names, such as ": instruction#7, instruction#9", for messages.
  std::string names() const {
    std::string s;
    for (std::size_t i = 0; i < members_.size() && i < 8; ++i) s += (i ? ", " : ": ") + detail::describe(*members_[i]);
    if (members_.size() > 8) s += ", ...";
    return s;
  }

  std::vector<T*> members_;
  std::string what_;
};

// A set of objects that keeps the order they joined in. Queries see the members from before the clock edge; add and
// remove are effects of the next event emitted on the object.
template <class T>
class Collection {
 public:
  explicit Collection(const char* name) : impl_(detail::make_collection<T>(name)) {}
  Collection(const Collection&) = delete;
  Collection& operator=(const Collection&) = delete;

  View<T> view() const { return View<T>(impl_.members(), impl_.name()); }
  template <class P>
  View<T> where(P&& pred) const {
    return view().where(std::forward<P>(pred));
  }
  template <class K>
  View<T> order_by(K&& key) const {
    return view().order_by(std::forward<K>(key));
  }
  T& only() const { return view().only(); }
  T& head() const { return view().head(); }
  T& last() const { return view().last(); }
  T& at(std::size_t k) const { return view().at(k); }
  std::size_t size() const { return impl_.members().size(); }
  bool empty() const { return impl_.members().empty(); }
  // The members from before the clock edge, which no effect changes before the cycle's handlers have all run.
  typename View<T>::iterator begin() const { return typename View<T>::iterator(impl_.members().begin()); }
  typename View<T>::iterator end() const { return typename View<T>::iterator(impl_.members().end()); }

  void add(T& o);
  void remove(T& o);

 private:
  eventtrace::Collection<T> impl_;
};

namespace detail {

// The library's half of the runtime: the sites, the manifest, and the cycle's effects and records.
class Library final : public eventtrace::Sites, public eventtrace::Effects {
 public:
  static Library& get();

  // Declarations, in the order the design's C++ makes them.
  std::vector<std::unique_ptr<ObjectType>> types;
  std::vector<EventBase*> events;
  std::vector<Call*> calls;
  unsigned collection_count = 0;

  // The run.
  void open(const char* path, const char* start, const char* stop);
  void finish();
  bool recording() const { return runtime_ && runtime_->is_open(); }
  eventtrace::Runtime& runtime() { return *runtime_; }
  [[noreturn]] void fail(const std::string& message) const;

  // The handler running, if any.
  struct Running {
    const Call* call;
    unsigned index;
    std::vector<Object*> touched;
  };
  Running* running = nullptr;
  std::string context() const;

  void queue(long long cycle, CallSite where, std::function<void()> handler);
  unsigned site_of(const EventBase& event, bool blocked) const;

  // One record, buffered until the cycle's commit writes its records in site order.
  struct Record {
    unsigned site;
    unsigned long long subject;
    std::vector<uint64_t> values;
    std::vector<bool> is_signed;
    uint64_t absent;
  };
  std::vector<Record> records;

  void stage_assign(unsigned site, unsigned long long event, Object& o, const Object::PendingAssign& a) {
    assign(*runtime_, site, event, o, a.index, o.oet_type->fields[a.index].name.c_str(), a.set_once_taken, a.write);
  }
  template <class T>
  void stage_add(unsigned site, unsigned long long event, eventtrace::Collection<T>& c, T& o) {
    add(*runtime_, site, event, c, o);
  }
  template <class T>
  void stage_remove(unsigned site, unsigned long long event, eventtrace::Collection<T>& c, T& o) {
    remove(*runtime_, site, event, c, o);
  }
  template <class T>
  void stage_destroy(unsigned site, unsigned long long event, T& o) {
    destroy(*runtime_, site, event, store<T>(), o);
  }

  // eventtrace::Sites
  unsigned site_count() const override { return static_cast<unsigned>(site_events_.size()); }
  unsigned payload_words(unsigned) const override { return 0; }
  const eventtrace::EventInfo& event(unsigned site) const override { return infos_[site_events_[site].event]; }
  unsigned object_type_count() const override { return static_cast<unsigned>(types.size()); }
  const char* manifest_sha256() const override { return sha256_.c_str(); }
  void handle(eventtrace::Runtime&, unsigned, const uint32_t*) override;
  void commit(eventtrace::Runtime& rt) override;

  const std::string& manifest() const { return manifest_; }

 private:
  Library() = default;
  void finalize();
  void check_declarations() const;
  std::string build_manifest() const;

  std::optional<eventtrace::Runtime> runtime_;
  struct SiteEvent {
    unsigned event;  // index among the manifest's events
    bool blocks;
  };
  std::vector<SiteEvent> site_events_;
  std::vector<EventBase*> manifest_events_;  // the events with sites, sorted by name
  std::vector<eventtrace::EventInfo> infos_;
  std::vector<std::vector<eventtrace::FieldInfo>> field_infos_;
  std::vector<std::vector<eventtrace::EnumVariant>> variant_infos_;
  std::vector<long long> last_cycle_;  // per call slot, for the once-per-cycle rule
  std::vector<std::string> names_;     // the manifest events' names, which infos_ points into
  std::string manifest_, sha256_;
};

inline Library& library() { return Library::get(); }

template <class T>
eventtrace::Collection<T> make_collection(const char* name) {
  Library& lib = library();
  return eventtrace::Collection<T>(lib, lib.collection_count++, name);
}

}  // namespace detail

template <class V, detail::Property P, bool IsParent>
Field<V, P, IsParent>& Field<V, P, IsParent>::operator=(const V& v) {
  detail::Library& lib = detail::library();
  if (!lib.running) detail::fail("a field is assigned outside a handler");
  if (!owner_) detail::fail("a field is assigned that OET_OBJECT does not list, or of an object the library did not create");
  const detail::FieldDecl& decl = owner_->oet_type->fields[index_];
  if (!v.is_set()) detail::fail(detail::describe(*owner_) + "." + decl.name + " is assigned an unset value");
  if (P == detail::Property::Const && owner_->oet_created != detail::current_cycle())
    detail::fail(detail::describe(*owner_) + "." + decl.name + " is const, so only its create event assigns it");
  for (const Object::PendingAssign& a : owner_->oet_assigns)
    if (a.index == index_)
      detail::fail(detail::describe(*owner_) + "." + decl.name + " is assigned twice before an event is emitted on it");
  const bool taken = P == detail::Property::SetOnce && value_.is_set();
  owner_->oet_assigns.push_back({index_, v.encoded(), taken, [this, v] { value_ = v; }});
  detail::touch(*owner_);
  return *this;
}

template <class T>
void Collection<T>::add(T& o) {
  if (!detail::library().running) detail::fail(std::string("an object is added to ") + impl_.name() + " outside a handler");
  o.oet_moves.push_back([this, &o](unsigned site, unsigned long long event) {
    detail::library().stage_add(site, event, impl_, o);
  });
  detail::touch(o);
}

template <class T>
void Collection<T>::remove(T& o) {
  if (!detail::library().running)
    detail::fail(std::string("an object is removed from ") + impl_.name() + " outside a handler");
  o.oet_moves.push_back([this, &o](unsigned site, unsigned long long event) {
    detail::library().stage_remove(site, event, impl_, o);
  });
  detail::touch(o);
}

template <class T, class Values>
void Event<T, Values>::destroy_subject(unsigned site, unsigned long long event, T& subject) {
  if constexpr (kSubject) detail::library().stage_destroy(site, event, subject);
}

}  // namespace oet

// Declares an enum whose values are variants, as a ChiselEnum: OET_ENUM(Stage, Decode, Execute = 4, Writeback). A
// value converts from an integer explicitly, as Stage(raw). As in Chisel, a value from hardware that is no variant is
// recorded as it is, and emit checks only that it fits the enum's width.
#define OET_DETAIL_ENUM_VALUE(Name, x) static_cast<uint64_t>(((::oet::detail::IgnoreAssign<Variant>)Variant::x).value)
#define OET_ENUM(Name, ...)                                                                                      \
  struct Name {                                                                                                  \
    enum Variant : uint64_t { __VA_ARGS__ };                                                                     \
    constexpr Name() = default;                                                                                  \
    constexpr Name(Variant v) : v_(v), set_(true) {}                                                             \
    template <std::integral I>                                                                                   \
    explicit constexpr Name(I raw) : v_(static_cast<uint64_t>(raw)), set_(true) {}                               \
    constexpr bool is_set() const { return set_; }                                                               \
    constexpr uint64_t value() const { return v_; }                                                              \
    friend constexpr bool operator==(const Name& a, const Name& b) { return a.set_ && b.set_ && a.v_ == b.v_; }  \
    friend constexpr std::partial_ordering operator<=>(const Name& a, const Name& b) {                           \
      if (!a.set_ || !b.set_) return std::partial_ordering::unordered;                                           \
      return a.v_ <=> b.v_;                                                                                      \
    }                                                                                                            \
    static const ::oet::detail::EnumDecl* decl() {                                                               \
      static const ::oet::detail::EnumDecl d = ::oet::detail::make_enum(                                         \
          #Name, #__VA_ARGS__, {OET_DETAIL_FOR_EACH(OET_DETAIL_ENUM_VALUE, Name, __VA_ARGS__)});                 \
      return &d;                                                                                                 \
    }                                                                                                            \
    static ::oet::detail::Type type() { return {::oet::detail::Kind::Enum, decl()->width, &decl, nullptr}; }    \
    ::oet::detail::Encoded encoded() const { return {set_, v_, decl()->width == 64 || v_ >> decl()->width == 0}; } \
                                                                                                                 \
   private:                                                                                                      \
    uint64_t v_ = 0;                                                                                             \
    bool set_ = false;                                                                                           \
  }

namespace oet::detail {
// Reads an enumerator out of `Name = value` in OET_ENUM's list, ignoring the assignment.
template <class E>
struct IgnoreAssign {
  E value;
  constexpr IgnoreAssign(E v) : value(v) {}
  template <class A>
  constexpr const IgnoreAssign& operator=(const A&) const {
    return *this;
  }
};
}  // namespace oet::detail

// Registers an object type: its name in the trace, and its fields in declaration order, which must be oet::Const,
// oet::SetOnce, oet::Mutable, or oet::Parent members. OET_OBJECT(Instruction, "instruction", pc, bits, stage)
// goes in the namespace of the type, after it.
#define OET_DETAIL_MEMBER(Type, f) &Type::f
#define OET_OBJECT(Type, Name, ...)                                                                          \
  [[maybe_unused]] inline auto oet_object_decl(Type*) {                                                      \
    return ::oet::detail::object_decl(Name, #__VA_ARGS__, OET_DETAIL_FOR_EACH(OET_DETAIL_MEMBER, Type, __VA_ARGS__)); \
  }

// Registers the struct of an event's values beyond its subject's fields: every member, each a value type or a
// std::optional of one, which a record may leave out. OET_FIELDS(DecodeValues, stall) goes after the struct.
#define OET_FIELDS(Struct, ...)                                                                                  \
  [[maybe_unused]] inline std::vector<::oet::detail::ValueDecl> oet_fields_decl(Struct*) {                       \
    return ::oet::detail::fields_decl<Struct>(#__VA_ARGS__, OET_DETAIL_FOR_EACH(OET_DETAIL_MEMBER, Struct, __VA_ARGS__)); \
  }
