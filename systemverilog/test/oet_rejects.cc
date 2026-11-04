// Declarations that break a rule the library checks when the trace opens, one per build: compiled with -DCASE=n, the
// program must stop with case n's message. Rules the compiler enforces, such as a listed field's object type or a
// value's type, are not tested here.
#include <cstdio>

#include "oet.h"

extern "C" void oet_open(const char* path, const char* start, const char* stop);

struct Cell : oet::Object {
  oet::Const<oet::UInt<4>> key;
  oet::Mutable<oet::UInt<4>> value;
  oet::Mutable<oet::UInt<4>> unlisted;  // left out of OET_OBJECT
};
OET_OBJECT(Cell, "cell", key, value)

struct Twice : oet::Object {
  oet::Parent<Cell> a;
  oet::Parent<Cell> b;
};
OET_OBJECT(Twice, "twice", a, b)

struct Badly : oet::Object {
  oet::Mutable<oet::UInt<4>> cycle;
};
OET_OBJECT(Badly, "badly", cycle)

struct Spaced : oet::Object {
  oet::Mutable<oet::UInt<4>> v;
};
OET_OBJECT(Spaced, "spaced type", v)

struct ValueNamedLikeField { oet::UInt<4> value; };
OET_FIELDS(ValueNamedLikeField, value)

namespace first { OET_ENUM(Mode, Off, On); }
namespace second { OET_ENUM(Mode, Low, High); }
struct TwoModes { first::Mode a; second::Mode b; };
OET_FIELDS(TwoModes, a, b)

#if CASE == 1
#define WANT "two oet::Event declarations are named cell.poke; sites sharing an event share its declaration"
oet::Event<Cell> poke1{"poke", &Cell::value};
oet::Event<Cell> poke2{"poke", &Cell::value};
oet::Call c1{"c1", {poke1}}, c2{"c2", {poke2}};
#elif CASE == 2
#define WANT "event cell.poke-it: the name is not an identifier"
oet::Event<Cell> poke{"poke-it", &Cell::value};
oet::Call c{"c", {poke}};
#elif CASE == 3
#define WANT "field badly.cycle uses a reserved name; every record already has cycle, event_id, site_id, subject"
oet::Event<Badly> make{"make", oet::create};
oet::Call c{"c", {make}};
#elif CASE == 4
#define WANT "object type twice declares 2 parents; a type has at most one"
oet::Event<Twice> make{"make", oet::create, &Twice::a, &Twice::b};
oet::Call c{"c", {make}};
#elif CASE == 5
#define WANT "two enums are named Mode; enum names are unique"
oet::Event<oet::none, TwoModes> modes{"modes"};
oet::Call c{"c", {modes}};
#elif CASE == 6
#define WANT "event cell.make creates its subject, so it lists the const field key, and not as optional"
oet::Event<Cell> make{"make", oet::create, &Cell::value};
oet::Call c{"c", {make}};
#elif CASE == 7
#define WANT "event cell.rekey lists the const field key, which only a create event assigns"
oet::Event<Cell> rekey{"rekey", &Cell::key};
oet::Call c{"c", {rekey}};
#elif CASE == 8
#define WANT "event stalled declares oet::by<T>() but has no subject to block or abort"
oet::Event<> stalled{"stalled", oet::by<Cell>()};
oet::Call c{"c", {stalled}};
#elif CASE == 9
#define WANT "event ended has a lifecycle but no subject"
oet::Event<> ended{"ended", oet::complete};
oet::Call c{"c", {ended}};
#elif CASE == 10
#define WANT "call \"two words\": the name is not an identifier"
oet::Event<> tick{"tick"};
oet::Call c{"two words", {tick}};
#elif CASE == 11
#define WANT "call none has no indices"
oet::Event<> tick{"tick"};
oet::Call c{"none", 0, {tick}};
#elif CASE == 12
#define WANT "cell.poke lists a field that its subject's OET_OBJECT does not"
oet::Event<Cell> poke{"poke", &Cell::unlisted};
oet::Call c{"c", {poke}};
#elif CASE == 13
#define WANT "event cell.poke has two fields named value"
oet::Event<Cell, ValueNamedLikeField> poke{"poke", &Cell::value};
oet::Call c{"c", {poke}};
#elif CASE == 14
#define WANT "object type \"spaced type\": the name is not an identifier"
oet::Event<Spaced> make{"make", oet::create};
oet::Call c{"c", {make}};
#elif CASE == 15
#define WANT "queue q has 0 entries; a queue has at least 1"
oet::Queue<Cell> q{"q", 0};
#elif CASE == 16
#define WANT "ROB r has 0 entries; a ROB has at least 1"
oet::Rob<Cell> r{"r", 0, 4};
#elif CASE == 17
#define WANT "ROB r has 4 keys for 8 entries; it needs a key for each entry"
oet::Rob<Cell> r{"r", 8, 4};
#elif CASE == 18
#define WANT "map m has 0 entries; a map has at least 1"
oet::Map<Cell> m{"m", 0};
#endif

int main() {
  std::fprintf(stderr, "expect: %s\n", WANT);
  oet_open("build/oet_rejects.trace", "", "");
  std::fprintf(stderr, "the declarations were accepted\n");
  return 0;
}
