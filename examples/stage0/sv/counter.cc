// Stage 0's C++: one event without a subject, carrying two values.
#include "VTestTop__Dpi.h"  // Verilator's prototypes of the design's imports, so a mismatch fails to compile
#include "oet.h"

// The tick's two fields: the 32-bit counter, and its low byte as a signed value minus 20.
struct TickValues {
  oet::UInt<32> count;
  oet::SInt<8> delta;
};
OET_FIELDS(TickValues, count, delta)

oet::Event<oet::none, TickValues> tick{"tick"};
oet::Call tick_call{"tick", {tick}};

// Counter.sv's call for a tick: emits it with the two values the design passes.
void oet_tick(long long cycle, unsigned count, char delta) {
  oet::at(cycle, tick_call, [=] { tick.emit({.count = count, .delta = static_cast<int8_t>(delta)}); });
}
