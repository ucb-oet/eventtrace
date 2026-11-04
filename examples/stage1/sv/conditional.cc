// Stage 1's C++: an event per Chisel site, each its own call, in the order Conditional.scala elaborates them, so the
// sites keep the Chisel version's IDs.
#include "VTestTop__Dpi.h"  // Verilator's prototypes of the design's imports, so a mismatch fails to compile
#include "oet.h"

OET_ENUM(Mode,
         Off,
         Slow,
         Fast);

struct FirstValues { oet::UInt<8> count; };
OET_FIELDS(FirstValues, count)
struct SecondValues { oet::UInt<8> count; oet::UInt<8> sum; };
OET_FIELDS(SecondValues, count, sum)
struct RestValues { oet::UInt<2> phase; oet::Bool odd; oet::UInt<1> path; };
OET_FIELDS(RestValues, phase, odd, path)
struct ModeValues { Mode mode; Mode target; };
OET_FIELDS(ModeValues, mode, target)
struct WideValues { oet::UInt<32> a; oet::UInt<36> b; oet::UInt<8> c; };
OET_FIELDS(WideValues, a, b, c)
struct FlushValues { std::optional<oet::UInt<2>> cause; };
OET_FIELDS(FlushValues, cause)
struct LaneValues { oet::UInt<8> value; };
OET_FIELDS(LaneValues, value)

oet::Event<> tick{"tick"};
oet::Event<oet::none, FirstValues> first{"first"};
oet::Event<oet::none, SecondValues> second{"second"};
oet::Event<oet::none, RestValues> rest{"rest"};
oet::Event<oet::none, ModeValues> mode{"mode"};
oet::Event<oet::none, WideValues> wide{"wide"};
oet::Event<oet::none, FlushValues> flush{"flush"};
oet::Event<> rise1{"rise1"};
oet::Event<> rise2{"rise2"};
oet::Event<> fall2{"fall2"};
oet::Event<oet::none, LaneValues> lane{"lane"};

oet::Call tick_call{"tick", {tick}};
oet::Call first_call{"first", {first}};
oet::Call second_call{"second", {second}};
oet::Call rest_even_call{"rest_even", {rest}};
oet::Call rest_odd_call{"rest_odd", {rest}};
oet::Call mode_call{"mode", {mode}};
oet::Call wide_call{"wide", {wide}};
oet::Call flush_absent_call{"flush_absent", {flush}};
oet::Call flush_call{"flush", {flush}};
oet::Call rise1_call{"rise1", {rise1}};
oet::Call rise2_call{"rise2", {rise2}};
oet::Call fall2_call{"fall2", {fall2}};
oet::Call lane_call{"lane", 2, {lane}};  // one site per instance

void oet_tick(long long cycle) { oet::at(cycle, tick_call, [] { tick.emit(); }); }
void oet_first(long long cycle, unsigned char count) {
  oet::at(cycle, first_call, [=] { first.emit({.count = count}); });
}
void oet_second(long long cycle, unsigned char count, unsigned char sum) {
  oet::at(cycle, second_call, [=] { second.emit({.count = count, .sum = sum}); });
}
// The two sites of rest differ in `path`, which the handler gives.
void oet_rest_even(long long cycle, unsigned char phase, svBit odd) {
  oet::at(cycle, rest_even_call, [=] { rest.emit({.phase = phase, .odd = odd != 0, .path = 0}); });
}
void oet_rest_odd(long long cycle, unsigned char phase, svBit odd) {
  oet::at(cycle, rest_odd_call, [=] { rest.emit({.phase = phase, .odd = odd != 0, .path = 1}); });
}
void oet_mode(long long cycle, unsigned char raw) {
  oet::at(cycle, mode_call, [=] { mode.emit({.mode = Mode(raw), .target = Mode::Fast}); });
}
void oet_wide(long long cycle, unsigned a, unsigned long long b, unsigned char c) {
  oet::at(cycle, wide_call, [=] { wide.emit({.a = a, .b = b, .c = c}); });
}
void oet_flush_absent(long long cycle) { oet::at(cycle, flush_absent_call, [] { flush.emit({}); }); }
void oet_flush(long long cycle, unsigned char cause) {
  oet::at(cycle, flush_call, [=] { flush.emit({.cause = cause}); });
}
void oet_rise1(long long cycle) { oet::at(cycle, rise1_call, [] { rise1.emit(); }); }
void oet_rise2(long long cycle) { oet::at(cycle, rise2_call, [] { rise2.emit(); }); }
void oet_fall2(long long cycle) { oet::at(cycle, fall2_call, [] { fall2.emit(); }); }
void oet_lane(long long cycle, int index, unsigned char value) {
  oet::at(cycle, lane_call[index], [=] { lane.emit({.value = value}); });
}
