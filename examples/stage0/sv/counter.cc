// Stage 0's C++: one event without a subject, carrying two values.
#include "VTestTop__Dpi.h"  // Verilator's prototypes of the design's imports, so a mismatch fails to compile
#include "oet.h"

struct TickValues {
  oet::UInt<32> count;
  oet::SInt<8> delta;
};
OET_FIELDS(TickValues, count, delta)

oet::Event<oet::none, TickValues> tick{"tick"};
oet::Call tick_call{"tick", {tick}};

void oet_tick(long long cycle, unsigned count, char delta) {
  oet::at(cycle, tick_call, [=] { tick.emit({.count = count, .delta = static_cast<int8_t>(delta)}); });
}
