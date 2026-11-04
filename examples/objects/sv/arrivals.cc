// Objects' C++: two calls create packets through one event, packet.arrive, and a third creates tokens.
#include "VTestTop__Dpi.h"  // Verilator's prototypes of the design's imports, so a mismatch fails to compile
#include "oet.h"

struct Packet : oet::Object {
  oet::Const<oet::UInt<8>> seq;
  oet::SetOnce<oet::UInt<2>> kind;
};
OET_OBJECT(Packet, "packet", seq, kind)

struct Token : oet::Object {
  oet::Const<oet::UInt<4>> slot;
};
OET_OBJECT(Token, "token", slot)

struct ArriveValues {
  oet::UInt<1> lane;
};
OET_FIELDS(ArriveValues, lane)

// Packets from every 8th cycle have no kind, so the event lists kind as optional.
oet::Event<Packet, ArriveValues> arrive{"arrive", oet::create, &Packet::seq, oet::optional(&Packet::kind)};
oet::Event<Token> grant{"grant", oet::create, &Token::slot};

oet::Call arrive4_call{"arrive4", {arrive}};
oet::Call arrive8_call{"arrive8", {arrive}};
oet::Call grant_call{"grant", {grant}};

void oet_arrive4(long long cycle, unsigned char seq, unsigned char kind) {
  oet::at(cycle, arrive4_call, [=] {
    Packet& p = arrive.create();
    p.seq = seq;
    p.kind = kind;
    arrive.emit(p, {.lane = 0});
  });
}
void oet_arrive8(long long cycle, unsigned char seq) {
  oet::at(cycle, arrive8_call, [=] {
    Packet& p = arrive.create();
    p.seq = seq;
    arrive.emit(p, {.lane = 1});
  });
}
void oet_grant(long long cycle, unsigned char slot) {
  oet::at(cycle, grant_call, [=] {
    Token& t = grant.create();
    t.slot = slot;
    grant.emit(t);
  });
}
