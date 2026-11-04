// oet: EventTrace for designs that call into C++ at each traced point, such as a SystemVerilog design through DPI.
//
// The design's C++ declares object types, enums, collections, events, and calls, and each DPI function queues a
// handler with oet::at. The handler queries collections and emits events with Chisel's semantics: queries read the
// state from before the clock edge, and an event's effects hold from the next cycle. The library writes the trace
// through the shared runtime and builds the manifest from the declarations. See docs/systemverilog.md.
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
// OET_DETAIL_EXPAND(x) is x, rescanned, so that a __VA_ARGS__ passed through it splits into separate arguments.
#define OET_DETAIL_EXPAND(x) x
// OET_DETAIL_FE_n(m, d, x, ...) applies m to its n arguments: m(d, x), then OET_DETAIL_FE_(n-1) on the rest.
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
// OET_DETAIL_FE_PICK names its 65th argument: given the arguments and then FE_64 down to FE_1, it names FE_n for n
// arguments.
#define OET_DETAIL_FE_PICK(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, _33, _34, _35, _36, _37, _38, _39, _40, _41, _42, _43, _44, _45, _46, _47, _48, _49, _50, _51, _52, _53, _54, _55, _56, _57, _58, _59, _60, _61, _62, _63, _64, NAME, ...) NAME
// OET_DETAIL_FOR_EACH picks the OET_DETAIL_FE_n for its n arguments after m and d, and applies it.
#define OET_DETAIL_FOR_EACH(m, d, ...) \
  OET_DETAIL_EXPAND(OET_DETAIL_FE_PICK(__VA_ARGS__, OET_DETAIL_FE_64, OET_DETAIL_FE_63, OET_DETAIL_FE_62, OET_DETAIL_FE_61, OET_DETAIL_FE_60, OET_DETAIL_FE_59, OET_DETAIL_FE_58, OET_DETAIL_FE_57, OET_DETAIL_FE_56, OET_DETAIL_FE_55, OET_DETAIL_FE_54, OET_DETAIL_FE_53, OET_DETAIL_FE_52, OET_DETAIL_FE_51, OET_DETAIL_FE_50, OET_DETAIL_FE_49, OET_DETAIL_FE_48, OET_DETAIL_FE_47, OET_DETAIL_FE_46, OET_DETAIL_FE_45, OET_DETAIL_FE_44, OET_DETAIL_FE_43, OET_DETAIL_FE_42, OET_DETAIL_FE_41, OET_DETAIL_FE_40, OET_DETAIL_FE_39, OET_DETAIL_FE_38, OET_DETAIL_FE_37, OET_DETAIL_FE_36, OET_DETAIL_FE_35, OET_DETAIL_FE_34, OET_DETAIL_FE_33, OET_DETAIL_FE_32, OET_DETAIL_FE_31, OET_DETAIL_FE_30, OET_DETAIL_FE_29, OET_DETAIL_FE_28, OET_DETAIL_FE_27, OET_DETAIL_FE_26, OET_DETAIL_FE_25, OET_DETAIL_FE_24, OET_DETAIL_FE_23, OET_DETAIL_FE_22, OET_DETAIL_FE_21, OET_DETAIL_FE_20, OET_DETAIL_FE_19, OET_DETAIL_FE_18, OET_DETAIL_FE_17, OET_DETAIL_FE_16, OET_DETAIL_FE_15, OET_DETAIL_FE_14, OET_DETAIL_FE_13, OET_DETAIL_FE_12, OET_DETAIL_FE_11, OET_DETAIL_FE_10, OET_DETAIL_FE_9, OET_DETAIL_FE_8, OET_DETAIL_FE_7, OET_DETAIL_FE_6, OET_DETAIL_FE_5, OET_DETAIL_FE_4, OET_DETAIL_FE_3, OET_DETAIL_FE_2, OET_DETAIL_FE_1)(m, d, __VA_ARGS__))

namespace oet {

struct Object;
struct none {};  // the subject of an event without one, or the values of an event with none of its own
template <class T>
class Id;

namespace detail {

// A value's kind, as the manifest's "type" names it.
enum class Kind { Bool, UInt, SInt, Enum, Id };
// A field's property, as the manifest's "property" names it: const, setOnce, or mutable.
enum class Property { Const, SetOnce, Mutable };
// What an event does to its subject's life: nothing, or create, complete, or abort it.
enum class Lifecycle { None, Create, Complete, Abort };

// An enum's name and its variants' names and values, in declaration order.
struct EnumDecl {
  std::string name;
  std::vector<std::pair<std::string, uint64_t>> variants;
  unsigned width = 1;  // enough for the largest value, as a ChiselEnum's
};
// OET_ENUM's declaration, from its name, its variant list as written, such as "A, B = 4", and the variants' values
// in order. Its width fits the largest value.
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

// Whether `v` is below zero, without comparing an unsigned type with 0, which -Wextra warns about.
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

// A 1-bit value, as Chisel's Bool. A field declared as Const<bool>, SetOnce<bool>, or Mutable<bool> holds one.
class Bool {
 public:
  // Unset.
  constexpr Bool() = default;
  // Set to `v`.
  constexpr Bool(bool v) : v_(v), set_(true) {}
  // Whether a value was given; a default-constructed one has none.
  constexpr bool is_set() const { return set_; }
  // The value; false when unset.
  constexpr bool value() const { return v_; }
  // True only when both are set and equal, so != is true whenever either is unset.
  friend constexpr bool operator==(const Bool& a, const Bool& b) { return a.set_ && b.set_ && a.v_ == b.v_; }
  // Unordered when either is unset, so <, <=, >, and >= are false.
  friend constexpr std::partial_ordering operator<=>(const Bool& a, const Bool& b) {
    if (!a.set_ || !b.set_) return std::partial_ordering::unordered;
    return a.v_ <=> b.v_;
  }
  // The manifest's type of a Bool.
  static detail::Type type() { return {detail::Kind::Bool, 1, nullptr, nullptr}; }
  // The value as a record holds it; a Bool always fits.
  detail::Encoded encoded() const { return {set_, v_, true}; }

 private:
  bool v_ = false, set_ = false;
};

// An N-bit unsigned value, 1 to 64 bits wide, as Chisel's UInt.
template <unsigned N>
class UInt {
  static_assert(N >= 1 && N <= 64, "a value is 1 to 64 bits wide");

 public:
  // Unset.
  constexpr UInt() = default;
  // Set to `v`, from any integer type. A record carrying it checks that it fits: neither negative nor wider than N
  // bits.
  template <std::integral I>
  constexpr UInt(I v) : v_(static_cast<uint64_t>(v)), set_(true), negative_(detail::negative(v)) {}
  // Whether a value was given; a default-constructed one has none.
  constexpr bool is_set() const { return set_; }
  // The value's bits; 0 when unset.
  constexpr uint64_t value() const { return v_; }
  // True only when both are set and equal, so != is true whenever either is unset.
  friend constexpr bool operator==(const UInt& a, const UInt& b) { return a.set_ && b.set_ && a.v_ == b.v_; }
  // Unordered when either is unset, so <, <=, >, and >= are false.
  friend constexpr std::partial_ordering operator<=>(const UInt& a, const UInt& b) {
    if (!a.set_ || !b.set_) return std::partial_ordering::unordered;
    return a.v_ <=> b.v_;
  }
  // The manifest's type: UInt of width N.
  static detail::Type type() { return {detail::Kind::UInt, N, nullptr, nullptr}; }
  // The value as a record holds it, which fits when it is not negative and has no bits above N.
  detail::Encoded encoded() const {
    bool fits = !negative_;
    if constexpr (N < 64) fits = fits && v_ >> N == 0;  // a 64-bit value always fits, and shifting by 64 is undefined
    return {set_, v_, fits};
  }

 private:
  uint64_t v_ = 0;
  bool set_ = false, negative_ = false;
};

// An N-bit signed value, 1 to 64 bits wide, as Chisel's SInt.
template <unsigned N>
class SInt {
  static_assert(N >= 1 && N <= 64, "a value is 1 to 64 bits wide");

 public:
  // Unset.
  constexpr SInt() = default;
  // Set to `v`, from any integer type. A record carrying it checks that it fits N signed bits, which an unsigned `v`
  // above INT64_MAX never does.
  template <std::integral I>
  constexpr SInt(I v)
      : v_(static_cast<int64_t>(v)), set_(true),
        huge_(!std::is_signed_v<I> && static_cast<uint64_t>(v) > static_cast<uint64_t>(INT64_MAX)) {}
  // Whether a value was given; a default-constructed one has none.
  constexpr bool is_set() const { return set_; }
  // The value; 0 when unset.
  constexpr int64_t value() const { return v_; }
  // True only when both are set and equal, so != is true whenever either is unset.
  friend constexpr bool operator==(const SInt& a, const SInt& b) { return a.set_ && b.set_ && a.v_ == b.v_; }
  // Unordered when either is unset, so <, <=, >, and >= are false.
  friend constexpr std::partial_ordering operator<=>(const SInt& a, const SInt& b) {
    if (!a.set_ || !b.set_) return std::partial_ordering::unordered;
    return a.v_ <=> b.v_;
  }
  // The manifest's type: SInt of width N.
  static detail::Type type() { return {detail::Kind::SInt, N, nullptr, nullptr}; }
  // The value as a record holds it, in two's complement, which fits when it lies within N signed bits.
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
  // Unset.
  constexpr Id() = default;
  // The ID of object `o`.
  Id(const T& o) : v_(o.id), set_(true) {}
  // The ID `id`, such as one the hardware holds.
  constexpr Id(unsigned long long id) : v_(id), set_(true) {}
  // Whether an ID was given; a default-constructed one has none.
  constexpr bool is_set() const { return set_; }
  // The ID; 0 when unset.
  constexpr unsigned long long value() const { return v_; }
  // True only when both are set and equal, so != is true whenever either is unset.
  friend constexpr bool operator==(const Id& a, const Id& b) { return a.set_ && b.set_ && a.v_ == b.v_; }
  // Unordered when either is unset, so <, <=, >, and >= are false.
  friend constexpr std::partial_ordering operator<=>(const Id& a, const Id& b) {
    if (!a.set_ || !b.set_) return std::partial_ordering::unordered;
    return a.v_ <=> b.v_;
  }
  // The manifest's type: an ID of T's object type.
  static detail::Type type() { return {detail::Kind::Id, 0, nullptr, &detail::object_type<T>}; }
  // The ID as a record holds it; an ID always fits.
  detail::Encoded encoded() const { return {set_, v_, true}; }

 private:
  unsigned long long v_ = 0;
  bool set_ = false;
};

namespace detail {

// The value type that a field declared with V holds: V itself, or Bool for bool.
template <class V>
struct ValueOf {
  using type = V;
};
// A field declared with bool holds a Bool.
template <>
struct ValueOf<bool> {
  using type = Bool;
};

// A value type that <=> orders, which a field's ordering comparisons need.
template <class V>
concept Ordered = requires(const V& a) { a <=> a; };

// Whether `v`, a value or a field, holds a value; order_by checks each member's key with it.
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

  // Unset; the library binds it to its object when it creates the object.
  Field() = default;
  // Not copied: a field belongs to the object the library created it in.
  Field(const Field&) = delete;
  // Stages `v` as the field's write, carried by the next event emitted on the object. The run stops outside a
  // handler, for an unset `v`, for a const field of an object created in an earlier cycle, or for a second write
  // before an event is emitted on the object.
  Field& operator=(const V& v);
  // Stages the other field's value from before the clock edge, as operator=(const V&) does.
  Field& operator=(const Field& other) { return *this = other.value_; }

  // The value from before the clock edge.
  const V& get() const { return value_; }
  // Reads as get(), so that a field converts to its value and compares as it does.
  operator const V&() const { return value_; }
  // Whether the field held a value before the clock edge.
  bool is_set() const { return value_.is_set(); }

  // Compares the value from before the clock edge with `b`, as the value type does.
  friend bool operator==(const Field& a, const V& b) { return a.value_ == b; }
  // Orders the value from before the clock edge against `b`, as the value type does.
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

// A field that only the object's create event assigns, and which every create event lists.
template <class V>
using Const = Field<typename detail::ValueOf<V>::type, detail::Property::Const>;
// A field that keeps its first value: assigning it once it holds one is a violation.
template <class V>
using SetOnce = Field<typename detail::ValueOf<V>::type, detail::Property::SetOnce>;
// A field that any event on the object may assign, once per cycle.
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

// An object type as OET_OBJECT declares it: its name in the trace, and its fields in declaration order.
struct ObjectType {
  std::string name;
  unsigned index = 0;  // for the runtime's per-type ID counters
  std::vector<FieldDecl> fields;
  void (*bind)(Object&) = nullptr;  // tells each field of a new object its owner and index
};

// Gives `type` the next index and keeps it in the library, returning the kept copy, which lives for the run.
const ObjectType& register_object(ObjectType type);

// The library's access to a Field's private members.
struct FieldAccess {
  // Tells `f` the object it belongs to and its index among the object's fields.
  template <class F>
  static void bind(F& f, Object& owner, unsigned index) {
    f.owner_ = &owner;
    f.index_ = index;
  }
  // The value `f` held before the clock edge.
  template <class F>
  static const auto& value(const F& f) {
    return f.value_;
  }
};

// The names in a macro's stringified argument list, such as "a, b = 4, c", without spaces or "= value" parts.
std::vector<std::string> split_names(const char* names);

// What OET_OBJECT gives for a type: its name in the trace, its field names as written, and its field members.
template <class... Ms>
struct ObjectDecl {
  const char* name;
  const char* names;
  std::tuple<Ms...> members;
};
// An ObjectDecl, with the members' types deduced.
template <class... Ms>
ObjectDecl<Ms...> object_decl(const char* name, const char* names, Ms... members) {
  return {name, names, std::tuple<Ms...>(members...)};
}

// The value type, property, and parent flag of M, a field member pointer. It is defined only for oet::Field
// members, so OET_OBJECT rejects any other member.
template <class M>
struct MemberField;
// A pointer to an oet::Field member of C.
template <class C, class V, Property P, bool IsParent>
struct MemberField<Field<V, P, IsParent> C::*> {
  using value_type = V;
  static constexpr Property property = P;
  static constexpr bool is_parent = IsParent;
};

// T's object type, registered with the library on first use from T's OET_OBJECT, with a bind that tells a new
// object's fields their owner and index.
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
// AnyMember converts to any member's type, so it stands for any initializer.
struct AnyMember {
  // Converts to a reference to any type; declared only, for the requires expression below, which never calls it.
  template <class U>
  constexpr operator U&() const noexcept;
};
// Whether S can be brace-initialized from as many initializers as I has indices.
template <class S, std::size_t... I>
constexpr bool takes_initializers(std::index_sequence<I...>) {
  return requires { S{(void(I), AnyMember{})...}; };
}
// S's member count: counting up from N, the largest number of initializers S takes.
template <class S, std::size_t N = 0>
constexpr std::size_t member_count() {
  if constexpr (takes_initializers<S>(std::make_index_sequence<N + 1>{})) return member_count<S, N + 1>();
  else return N;
}

// How fields_decl reads a member of an event's value struct: a value type, which every record must set.
template <class M>
struct ValueMember {
  using value_type = M;
  static constexpr bool optional = false;
  // The member as a record holds it.
  static Encoded read(const M& v) { return v.encoded(); }
};
// A std::optional member: a value that a record may leave out.
template <class V>
struct ValueMember<std::optional<V>> {
  using value_type = V;
  static constexpr bool optional = true;
  // The member as a record holds it; unset when empty.
  static Encoded read(const std::optional<V>& v) { return v ? v->encoded() : Encoded{}; }
};

// One value of an event beyond its subject's fields: a member of the struct OET_FIELDS registers.
struct ValueDecl {
  std::string name;
  Type type;
  bool optional;
  std::function<Encoded(const void*)> read;
};

// The values of struct S, from OET_FIELDS's names and member pointers, which must list every member of S.
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

// The type of oet::create: the event creates its subject.
struct CreateTag {};
// The type of oet::complete: the event completes its subject, which then leaves its store.
struct CompleteTag {};
// The type of oet::abort: the event aborts its subject, which then leaves its store.
struct AbortTag {};
// The type of oet::by<T>(): records may name an object of type T as what blocks or aborted the subject.
template <class T>
struct ByTag {};
// The type of oet::optional(member): a listed subject field that some records leave unassigned.
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
  // A field write staged by Field's operator=, which the next event emitted on the object carries: the field's index,
  // the value it records, whether a setOnce field already held one, and the write applied at the end of the cycle.
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

// An event's lifecycle options: it creates, completes, or aborts its subject.
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
// As by(cause), with the cause given by its ID.
template <class T>
detail::Mode by(const Id<T>& cause) {
  return {detail::Mode::Kind::AbortedBy, cause.value(), &detail::object_type<T>};
}

// The record finds its subject blocked, by an unknown cause or by `cause`.
inline detail::Mode blocked() { return {detail::Mode::Kind::Blocked, std::nullopt, nullptr}; }
// The record finds its subject blocked by `cause`, of the type the event's oet::by<T>() declares.
template <class T>
  requires std::derived_from<T, Object>
detail::Mode blocked(const T& cause) {
  return {detail::Mode::Kind::Blocked, cause.id, &detail::object_type<T>};
}
// As blocked(cause), with the cause given by its ID.
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
  // Registers the event, named `name`, with the library.
  explicit EventBase(const char* name);
  // Neither copied nor assigned: the library and calls hold the event by address.
  EventBase(const EventBase&) = delete;
  EventBase& operator=(const EventBase&) = delete;

  std::string full_name() const;  // the manifest's name, qualified by the subject's type

  std::string name;
  const ObjectType& (*subject)() = nullptr;
  Lifecycle lifecycle = Lifecycle::None;
  const ObjectType& (*cause)() = nullptr;  // from oet::by<T>()
  // A subject field the event lists: its index among the subject's fields, and whether records may leave it out.
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
  // A new object of `type` for this create event, which `make` constructs in its store under the runtime's next ID
  // for the type. The run stops outside a handler or for an event not declared oet::create.
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
  // Declares the event `name`, registered with the library. The options are oet::create, oet::complete, or
  // oet::abort; oet::by<C>(); and the subject fields its records assign, as member pointers or oet::optional(...).
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

  // Emits a record on `subject` with the fields the event lists, as the handler assigned them, and stages the
  // object's waiting writes and collection changes; a complete or abort event also ends the subject.
  void emit(T& subject)
    requires kSubject
  {
    emit_on(subject, nullptr, {});
  }
  // As emit(subject), blocked or naming what aborted the subject, as `mode` gives.
  void emit(T& subject, const detail::Mode& mode)
    requires kSubject
  {
    emit_on(subject, nullptr, mode);
  }
  // As emit(subject), with the event's own values `v`.
  void emit(T& subject, const Values& v)
    requires(kSubject && kValues)
  {
    emit_on(subject, &v, {});
  }
  // As emit(subject, mode), with the event's own values `v`.
  void emit(T& subject, const Values& v, const detail::Mode& mode)
    requires(kSubject && kValues)
  {
    emit_on(subject, &v, mode);
  }
  // Emits a record of an event without a subject, with none of its values set.
  void emit()
    requires(!kSubject)
  {
    emit_record(nullptr, kValues ? &kNoValues : nullptr, {}, nullptr);
  }
  // Emits a record of an event without a subject, carrying `v`.
  void emit(const Values& v)
    requires(!kSubject && kValues)
  {
    emit_record(nullptr, &v, {}, nullptr);
  }

 private:
  // oet::create: the event creates its subject.
  void option(detail::CreateTag) { lifecycle = detail::Lifecycle::Create; }
  // oet::complete: the event completes its subject.
  void option(detail::CompleteTag) { lifecycle = detail::Lifecycle::Complete; }
  // oet::abort: the event aborts its subject.
  void option(detail::AbortTag) { lifecycle = detail::Lifecycle::Abort; }
  // oet::by<C>(): records may name an object of type C as their cause.
  template <class C>
  void option(detail::ByTag<C>) {
    cause = &detail::object_type<C>;
  }
  // A member pointer: a subject field that every record assigns.
  template <class M>
  void option(M T::*member)
    requires kSubject
  {
    listed.push_back({detail::field_index(member), false});
  }
  // oet::optional(member): a subject field that some records leave unassigned.
  template <class M>
  void option(detail::OptionalField<M T::*> f)
    requires kSubject
  {
    listed.push_back({detail::field_index(f.member), true});
  }

  // Emits on `subject` with values `v`, or with none set when the event has values and `v` is null.
  void emit_on(T& subject, const Values* v, const detail::Mode& mode) {
    const void* values_ptr = nullptr;
    if constexpr (kValues) values_ptr = v ? static_cast<const void*>(v) : static_cast<const void*>(&kNoValues);
    emit_record(&subject, values_ptr, mode, [&subject](unsigned site, unsigned long long event) {
      destroy_subject(site, event, subject);
    });
  }
  // Stages the end of `subject`, which leaves its store at the end of the cycle.
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
  // Declares the call `name`, with one index, which emits `events`, registered with the library.
  Call(const char* name, Events events);
  // Declares the call `name` with `count` indices, such as a channel's slots or a module's instances.
  Call(const char* name, unsigned count, Events events);
  // Neither copied nor assigned: the library and each CallSite hold the call by address.
  Call(const Call&) = delete;
  Call& operator=(const Call&) = delete;

  // The call at `index`, for oet::at.
  CallSite operator[](unsigned index) const { return {this, index, true}; }

  std::string name;
  unsigned count;
  std::vector<detail::EventBase*> events;

  // Set when the trace opens: the first handler order of its indices, and each index's sites per event.
  unsigned first_slot = 0;
  std::vector<std::vector<unsigned>> sites;  // [index][event]: the non-blocking site; the blocking one follows it
};

namespace detail {

// Whether the trace is open, so that oet::at queues handlers.
bool recording();
// Queues `handler` for `cycle` at `where`, as Library::queue does.
void queue(long long cycle, CallSite where, std::function<void()> handler);
// Notes that the running handler touched `o`, whose staged effects must then go with an event emitted on it.
void touch(Object& o);
// The cycle whose handlers are running.
long long current_cycle();
// `o` as messages name it, such as instruction#7.
std::string describe(const Object& o);

// A collection's declaration, which the library checks when the trace opens.
struct CollectionDecl {
  enum class Kind { Set, Queue, Rob, Map } kind;
  std::string name;
  std::optional<std::size_t> entries;  // none for a set, or a queue declared without entries
  uint64_t keys = 0;                   // a ROB's
};

// The tracker's collection C, registered as `decl` describes it; `sizes` are C's constructor arguments after its
// name.
template <class C, class... Sizes>
C make_collection(const CollectionDecl& decl, const char* name, Sizes... sizes);

}  // namespace detail

// Queues `handler` for `cycle`: it runs once the cycle is complete, with the other handlers of the cycle in the
// order their calls were declared, and reads the state from before the clock edge. Without +eventtrace, it does
// nothing.
template <class F>
void at(long long cycle, CallSite where, F&& handler) {
  if (!detail::recording()) return;
  detail::queue(cycle, where, std::function<void()>(std::forward<F>(handler)));
}
// As at(cycle, where, handler), for a call with one index.
template <class F>
void at(long long cycle, const Call& call, F&& handler) {
  at(cycle, CallSite{&call, 0, false}, std::forward<F>(handler));
}

// Stops the run with `message`, naming the running handler's call and cycle, as the library's own checks do: for
// the design's own checks, such as comparing the trace's state with the hardware's.
[[noreturn]] void fail(const std::string& message);

// Members of a collection, as a query has narrowed or ordered them, in order.
template <class T>
class View {
 public:
  // The members `members`, in order, named `what` in messages.
  View(std::vector<T*> members, std::string what) : members_(std::move(members)), what_(std::move(what)) {}

  // The members for which `pred` holds, in order.
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

  // The one member; the run stops if there are none or several.
  T& only() const {
    if (members_.size() != 1)
      detail::fail("only found " + std::to_string(members_.size()) + " members of " + what_ + names());
    return *members_.front();
  }
  // The first member; the run stops if there is none.
  T& head() const {
    if (members_.empty()) detail::fail("head found no members of " + what_);
    return *members_.front();
  }
  // The last member; the run stops if there is none.
  T& last() const {
    if (members_.empty()) detail::fail("last found no members of " + what_);
    return *members_.back();
  }
  // Member `k`, counting from 0; the run stops if there are not that many.
  T& at(std::size_t k) const {
    if (k >= members_.size())
      detail::fail("at(" + std::to_string(k) + ") found only " + std::to_string(members_.size()) + " members of " +
                   what_ + names());
    return *members_[k];
  }
  // The number of members.
  std::size_t size() const { return members_.size(); }
  // Whether the view has no members.
  bool empty() const { return members_.empty(); }

  // Iterates over the members as references, so that a range-for loop sees T&.
  class iterator {
   public:
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    // Singular, as the standard iterator concepts require.
    iterator() = default;
    // At `i` among the view's member pointers.
    explicit iterator(typename std::vector<T*>::const_iterator i) : i_(i) {}
    // The member.
    T& operator*() const { return **i_; }
    // The member's address, for ->.
    T* operator->() const { return *i_; }
    // Moves to the next member.
    iterator& operator++() {
      ++i_;
      return *this;
    }
    // Moves to the next member, returning the iterator from before.
    iterator operator++(int) {
      iterator old = *this;
      ++i_;
      return old;
    }
    // Equal at the same member.
    bool operator==(const iterator&) const = default;

   private:
    typename std::vector<T*>::const_iterator i_;
  };
  // At the first member.
  iterator begin() const { return iterator(members_.begin()); }
  // Past the last member.
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

namespace detail {

// The queries every kind of collection takes: those of a view of its members from before the clock edge, in the
// kind's order. Self holds the tracker's collection as impl_, which no effect changes before the cycle's handlers
// have all run.
template <class T, class Self>
class Queries {
 public:
  // The members from before the clock edge, in the kind's order, as a view named after the collection.
  View<T> view() const { return View<T>(members(), name()); }
  // The members for which `pred` holds, as View::where.
  template <class P>
  View<T> where(P&& pred) const {
    return view().where(std::forward<P>(pred));
  }
  // The members ranked by `key`, as View::order_by.
  template <class K>
  View<T> order_by(K&& key) const {
    return view().order_by(std::forward<K>(key));
  }
  // The one member, as View::only.
  T& only() const { return view().only(); }
  // The first member, as View::head.
  T& head() const { return view().head(); }
  // The last member, as View::last.
  T& last() const { return view().last(); }
  // Member `k`, as View::at.
  T& at(std::size_t k) const { return view().at(k); }
  // The number of members.
  std::size_t size() const { return members().size(); }
  // Whether there are no members.
  bool empty() const { return members().empty(); }
  // At the first member, for range-for.
  typename View<T>::iterator begin() const { return typename View<T>::iterator(members().begin()); }
  // Past the last member.
  typename View<T>::iterator end() const { return typename View<T>::iterator(members().end()); }

 protected:
  // Only a collection constructs one, as its base.
  Queries() = default;
  // Neither copied nor assigned, so neither is a collection, whose tracker keeps its address.
  Queries(const Queries&) = delete;
  Queries& operator=(const Queries&) = delete;

  // The tracker collection's members, which no effect changes until the cycle's handlers have all run.
  const std::vector<T*>& members() const { return static_cast<const Self&>(*this).impl_.members(); }
  // The collection's name, for messages.
  std::string name() const { return static_cast<const Self&>(*this).impl_.name(); }
};

// What a queue and a ROB share besides their queries: members leave from the head, through pop, or all at once,
// through clear. Self holds a tracker Queue or Rob as impl_.
template <class T, class Self>
class QueueOps : public Queries<T, Self> {
 public:
  // The head, which leaves the queue with the next event emitted on it; or, for the k-th of a cycle's pops counting
  // from 0, member k. The pops of one cycle must take the first members, as many as there are pops.
  T& pop(std::size_t k = 0);

  // Every member, each of which leaves with the next event emitted on it.
  View<T> clear();

 protected:
  // Stages the removal of each of `members`, by clear or rollback, and returns them as the view `what`.
  View<T> leaving(std::vector<T*> members, const std::string& what);

  // The tracker's Queue or Rob that Self holds.
  auto& impl() { return static_cast<Self&>(*this).impl_; }
};

}  // namespace detail

// A set of objects that keeps the order they joined in. Queries see the members from before the clock edge; add and
// remove are effects of the next event emitted on the object.
template <class T>
class Collection : public detail::Queries<T, Collection<T>> {
 public:
  // Declares the set `name`, registered with the library.
  explicit Collection(const char* name)
      : impl_(detail::make_collection<eventtrace::Collection<T>>({detail::CollectionDecl::Kind::Set, name}, name)) {}

  // o joins the set, last in join order, with the next event emitted on it.
  void add(T& o);
  // o leaves the set with the next event emitted on it.
  void remove(T& o);

 private:
  friend class detail::Queries<T, Collection>;
  eventtrace::Collection<T> impl_;
};

// A FIFO: members join at the tail and leave from the head, all at once, or from the tail as its youngest members.
// Declared with `entries`, it may hold at most that many members at the end of a cycle. Queries see the members
// oldest first.
template <class T>
class Queue : public detail::QueueOps<T, Queue<T>> {
 public:
  // Declares the queue `name`, with no bound on its members.
  explicit Queue(const char* name)
      : impl_(detail::make_collection<eventtrace::Queue<T>>({detail::CollectionDecl::Kind::Queue, name}, name,
                                                             std::size_t{0})) {}
  // Declares the queue `name`, which may hold at most `entries` members at the end of a cycle.
  Queue(const char* name, std::size_t entries)
      : impl_(detail::make_collection<eventtrace::Queue<T>>({detail::CollectionDecl::Kind::Queue, name, entries},
                                                             name, entries)) {}

  // o joins the tail with the next event emitted on it.
  void add(T& o);

  // The k youngest members, k being the hardware's count of them, each of which leaves with the next event emitted on
  // it. The run stops if there are fewer than k.
  View<T> drop_youngest(std::size_t k);

 private:
  friend class detail::Queries<T, Queue>;
  friend class detail::QueueOps<T, Queue>;
  eventtrace::Queue<T> impl_;
};

// A queue of at most `entries` members whose tail allocates keys, as a reorder buffer's tail allocates tags. Keys
// count modulo `keys`, the size of the tag space. Queries see the members oldest first.
template <class T>
class Rob : public detail::QueueOps<T, Rob<T>> {
 public:
  // Declares the ROB `name` of at most `entries` members, its keys counting modulo `keys`, which must be at least
  // `entries`.
  Rob(const char* name, std::size_t entries, uint64_t keys)
      : impl_(detail::make_collection<eventtrace::Rob<T>>({detail::CollectionDecl::Kind::Rob, name, entries, keys},
                                                           name, entries, keys)) {}

  // o joins the tail with `key`, the hardware's tag, with the next event emitted on it. The key must be the one the
  // tail allocates next; the first addition sets where allocation starts.
  void add(uint64_t key, T& o);

  // The live member with `key`; the run stops if there is none.
  T& operator()(uint64_t key) const;

  // Moves the tail back to `new_tail`, the value the hardware writes into its tail pointer: the members from the one
  // with key `new_tail` through the youngest, each of which leaves with the next event emitted on it, and allocation
  // continues from `new_tail`. `new_tail` may also be the key the tail allocates next, which removes nothing; any
  // other key stops the run.
  View<T> rollback(uint64_t new_tail);

 private:
  friend class detail::Queries<T, Rob>;
  friend class detail::QueueOps<T, Rob>;
  eventtrace::Rob<T> impl_;
};

// Members under keys below `entries` that the hardware chooses, as from a free list. Queries see the members in the
// order they joined.
template <class T>
class Map : public detail::Queries<T, Map<T>> {
 public:
  // Declares the map `name`, with keys from 0 to `entries` - 1.
  Map(const char* name, std::size_t entries)
      : impl_(detail::make_collection<eventtrace::Map<T>>({detail::CollectionDecl::Kind::Map, name, entries}, name,
                                                           entries)) {}

  // o joins under `key` with the next event emitted on it. The key must be below the entries and free once the
  // cycle's removals apply.
  void add(uint64_t key, T& o);

  // o leaves with the next event emitted on it.
  void remove(T& o);

  // The member under `key`; the run stops if there is none.
  T& operator()(uint64_t key) const;

 private:
  friend class detail::Queries<T, Map>;
  eventtrace::Map<T> impl_;
};

namespace detail {

// The library's half of the runtime: the sites, the manifest, and the cycle's effects and records.
class Library final : public eventtrace::Sites, public eventtrace::Effects {
 public:
  // The one library, created on first use and never destroyed.
  static Library& get();

  // Declarations, in the order the design's C++ makes them.
  std::vector<std::unique_ptr<ObjectType>> types;
  std::vector<EventBase*> events;
  std::vector<Call*> calls;
  std::vector<CollectionDecl> collections;  // by index

  // The run.
  // Opens the trace at `path`, with the recording window's triggers, and writes the manifest beside it as
  // <path>.json; the declarations are final from then on. Only the first call does anything, and the trace finishes
  // at exit if not before.
  void open(const char* path, const char* start, const char* stop);
  // Handles the last cycle and ends the trace; later calls do nothing.
  void finish();
  // Whether the trace is open.
  bool recording() const { return runtime_ && runtime_->is_open(); }
  // The runtime, once the trace is open.
  eventtrace::Runtime& runtime() { return *runtime_; }
  // Stops the run with `message`, after the running handler's context, through the runtime when the trace is open,
  // so that what was written is flushed.
  [[noreturn]] void fail(const std::string& message) const;

  // The handler running, if any.
  struct Running {
    const Call* call;
    unsigned index;
    std::vector<Object*> touched;
  };
  Running* running = nullptr;
  // The running handler's event, site, call, and cycle, as a prefix for messages; empty outside a handler.
  std::string context() const;

  // Queues `handler` for `cycle` in its call slot's order. The run stops for a missing or out-of-range index or a
  // second call in one cycle, and, after the handler, for an object it touched with effects no event carried or a
  // create event it did not emit.
  void queue(long long cycle, CallSite where, std::function<void()> handler);
  // The site of `event` for the running handler's call and index: the non-blocking one, or the blocking one after
  // it. The run stops if the call does not declare the event.
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

  // Stages a field write that a record carries, through the tracker's checks.
  void stage_assign(unsigned site, unsigned long long event, Object& o, const Object::PendingAssign& a) {
    assign(*runtime_, site, event, o, a.index, o.oet_type->fields[a.index].name.c_str(), a.set_once_taken, a.write);
  }
  // Stages `o` joining set `c`.
  template <class T>
  void stage_add(unsigned site, unsigned long long event, eventtrace::Collection<T>& c, T& o) {
    add(*runtime_, site, event, c, o);
  }
  // Stages `o` leaving set `c`.
  template <class T>
  void stage_remove(unsigned site, unsigned long long event, eventtrace::Collection<T>& c, T& o) {
    remove(*runtime_, site, event, c, o);
  }
  // Stages `o` joining queue `q` at its tail.
  template <class T>
  void stage_add(unsigned site, unsigned long long event, eventtrace::Queue<T>& q, T& o) {
    add(*runtime_, site, event, q, o);
  }
  // Stages `o` joining ROB `r` at its tail with `key`.
  template <class T>
  void stage_add(unsigned site, unsigned long long event, eventtrace::Rob<T>& r, T& o, uint64_t key) {
    add(*runtime_, site, event, r, o, key);
  }
  // Stages `o` joining map `m` under `key`.
  template <class T>
  void stage_add(unsigned site, unsigned long long event, eventtrace::Map<T>& m, T& o, uint64_t key) {
    add(*runtime_, site, event, m, o, key);
  }
  // Stages `o` leaving map `m`.
  template <class T>
  void stage_remove(unsigned site, unsigned long long event, eventtrace::Map<T>& m, T& o) {
    remove(*runtime_, site, event, m, o);
  }
  // Stages `o` leaving queue or ROB `q` from its head, by pop.
  template <class T>
  void stage_pop(unsigned site, unsigned long long event, eventtrace::Queue<T>& q, T& o) {
    pop(*runtime_, site, event, q, o);
  }
  // Stages `o` leaving queue or ROB `q` by clear, drop_youngest, or rollback.
  template <class T>
  void stage_drop(unsigned site, unsigned long long event, eventtrace::Queue<T>& q, T& o) {
    drop(*runtime_, site, event, q, o);
  }
  // Stages the end of `o`, which leaves its store at the end of the cycle.
  template <class T>
  void stage_destroy(unsigned site, unsigned long long event, T& o) {
    destroy(*runtime_, site, event, store<T>(), o);
  }

  // eventtrace::Sites
  // One site per call index and event, and a blocking site after each one whose event has a subject.
  unsigned site_count() const override { return static_cast<unsigned>(site_events_.size()); }
  // No site carries a payload: a handler emits its records in C++.
  unsigned payload_words(unsigned) const override { return 0; }
  // The manifest event that `site` records.
  const eventtrace::EventInfo& event(unsigned site) const override { return infos_[site_events_[site].event]; }
  // The declared object types, each with its own ID counter.
  unsigned object_type_count() const override { return static_cast<unsigned>(types.size()); }
  // SHA-256 of the manifest, computed when the trace opens.
  const char* manifest_sha256() const override { return sha256_.c_str(); }
  // Never called, as no record arrives with a payload; it stops the run.
  void handle(eventtrace::Runtime&, unsigned, const uint32_t*) override;
  // Writes the cycle's buffered records in site order, then applies the cycle's staged effects.
  void commit(eventtrace::Runtime& rt) override;

  // The manifest's JSON, built when the trace opens.
  const std::string& manifest() const { return manifest_; }

 private:
  // Only get() constructs it.
  Library() = default;
  // Completes the declarations when the trace opens: registers every object type they name, sorts the events with
  // sites by name and gives each its manifest fields, checks the rules, numbers the sites, and builds what the
  // recording window's triggers read, the manifest, and its hash.
  void finalize();
  // Stops the run on a declaration that breaks a rule the compiler cannot check: names, reserved and repeated field
  // names, field and parent counts, enums sharing a name, lifecycles and causes without a subject, how events list
  // const fields, call indices, and collection sizes.
  void check_declarations() const;
  // The manifest's JSON, laid out as ManifestGen writes it.
  std::string build_manifest() const;

  std::optional<eventtrace::Runtime> runtime_;
  // A site's event, as its index among the manifest's events, and whether the site's records find the subject
  // blocked.
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

// The library, as Library::get() returns it.
inline Library& library() { return Library::get(); }

template <class C, class... Sizes>
C make_collection(const CollectionDecl& decl, const char* name, Sizes... sizes) {
  Library& lib = library();
  const auto index = static_cast<unsigned>(lib.collections.size());
  lib.collections.push_back(decl);
  return C(lib, index, name, sizes...);  // constructed in place, so the tracker keeps its address
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

template <class T, class Self>
T& detail::QueueOps<T, Self>::pop(std::size_t k) {
  if (!detail::library().running) detail::fail("an object is popped from " + this->name() + " outside a handler");
  T& o = k == 0 ? this->head() : this->at(k);
  o.oet_moves.push_back([this, &o](unsigned site, unsigned long long event) {
    detail::library().stage_pop(site, event, impl(), o);
  });
  detail::touch(o);
  return o;
}

template <class T, class Self>
View<T> detail::QueueOps<T, Self>::clear() {
  if (!detail::library().running) detail::fail(this->name() + " is cleared outside a handler");
  return leaving(this->members(), this->name() + ".clear()");
}

template <class T, class Self>
View<T> detail::QueueOps<T, Self>::leaving(std::vector<T*> members, const std::string& what) {
  for (T* m : members) {
    m->oet_moves.push_back([this, m](unsigned site, unsigned long long event) {
      detail::library().stage_drop(site, event, impl(), *m);
    });
    detail::touch(*m);
  }
  return View<T>(std::move(members), what);
}

template <class T>
void Queue<T>::add(T& o) {
  if (!detail::library().running) detail::fail(std::string("an object is added to ") + impl_.name() + " outside a handler");
  o.oet_moves.push_back([this, &o](unsigned site, unsigned long long event) {
    detail::library().stage_add(site, event, impl_, o);
  });
  detail::touch(o);
}

template <class T>
View<T> Queue<T>::drop_youngest(std::size_t k) {
  const std::string what = std::string(impl_.name()) + ".drop_youngest(" + std::to_string(k) + ")";
  if (!detail::library().running) detail::fail(what + " is called outside a handler");
  const std::vector<T*>& members = impl_.members();
  if (k > members.size())
    detail::fail("drop_youngest(" + std::to_string(k) + ") found only " + std::to_string(members.size()) +
                 " members of " + impl_.name());
  return this->leaving(std::vector<T*>(members.end() - static_cast<std::ptrdiff_t>(k), members.end()), what);
}

template <class T>
void Rob<T>::add(uint64_t key, T& o) {
  if (!detail::library().running) detail::fail(std::string("an object is added to ") + impl_.name() + " outside a handler");
  o.oet_moves.push_back([this, &o, key](unsigned site, unsigned long long event) {
    detail::library().stage_add(site, event, impl_, o, key);
  });
  detail::touch(o);
}

template <class T>
T& Rob<T>::operator()(uint64_t key) const {
  T* o = impl_.find(key);
  if (!o) detail::fail(std::string(impl_.name()) + " has no member with key " + std::to_string(key));
  return *o;
}

template <class T>
View<T> Rob<T>::rollback(uint64_t new_tail) {
  if (!detail::library().running) detail::fail(std::string(impl_.name()) + " is rolled back outside a handler");
  std::optional<std::vector<T*>> members = impl_.following(new_tail);
  if (!members)
    detail::fail("rolls " + std::string(impl_.name()) + " back to key " + std::to_string(new_tail) +
                 ", which is neither a live key nor its tail, " + std::to_string(*impl_.next()));
  return this->leaving(std::move(*members), std::string(impl_.name()) + ".rollback(" + std::to_string(new_tail) + ")");
}

template <class T>
void Map<T>::add(uint64_t key, T& o) {
  if (!detail::library().running) detail::fail(std::string("an object is added to ") + impl_.name() + " outside a handler");
  o.oet_moves.push_back([this, &o, key](unsigned site, unsigned long long event) {
    detail::library().stage_add(site, event, impl_, o, key);
  });
  detail::touch(o);
}

template <class T>
void Map<T>::remove(T& o) {
  if (!detail::library().running)
    detail::fail(std::string("an object is removed from ") + impl_.name() + " outside a handler");
  o.oet_moves.push_back([this, &o](unsigned site, unsigned long long event) {
    detail::library().stage_remove(site, event, impl_, o);
  });
  detail::touch(o);
}

template <class T>
T& Map<T>::operator()(uint64_t key) const {
  T* o = impl_.find(key);
  if (!o) detail::fail(std::string(impl_.name()) + " has no member with key " + std::to_string(key));
  return *o;
}

template <class T, class Values>
void Event<T, Values>::destroy_subject(unsigned site, unsigned long long event, T& subject) {
  if constexpr (kSubject) detail::library().stage_destroy(site, event, subject);
}

}  // namespace oet

// Declares an enum whose values are variants, as a ChiselEnum: OET_ENUM(Stage, Decode, Execute = 4, Writeback). A
// value converts from an integer explicitly, as Stage(raw). As in Chisel, a value from hardware that is no variant is
// recorded as it is, and emit checks only that it fits the enum's width.
// OET_DETAIL_ENUM_VALUE(Name, x) is variant x's value, read through IgnoreAssign, which drops an `= value` in x.
#define OET_DETAIL_ENUM_VALUE(Name, x) static_cast<uint64_t>(((::oet::detail::IgnoreAssign<Variant>)Variant::x).value)
// OET_ENUM defines Name as a value type, as UInt is, whose width is that of its largest variant.
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
  // Wraps the enumerator `v`.
  constexpr IgnoreAssign(E v) : value(v) {}
  // Ignores the assigned value, so that `Variant::x = 4` reads as Variant::x.
  template <class A>
  constexpr const IgnoreAssign& operator=(const A&) const {
    return *this;
  }
};
}  // namespace oet::detail

// Registers an object type: its name in the trace, and its fields in declaration order, which must be oet::Const,
// oet::SetOnce, oet::Mutable, or oet::Parent members. OET_OBJECT(Instruction, "instruction", pc, bits, stage)
// goes in the namespace of the type, after it.
// OET_DETAIL_MEMBER(Type, f) is a pointer to member f, as OET_OBJECT and OET_FIELDS give each name.
#define OET_DETAIL_MEMBER(Type, f) &Type::f
// OET_OBJECT defines oet_object_decl(Type*), which object_type<Type>() finds by argument-dependent lookup.
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
