// Collections' C++: Pipeline.scala's sites as calls, in its order, each handler with its event block's queries,
// effects, and tests.
#include "VTestTop__Dpi.h"  // Verilator's prototypes of the design's imports, so a mismatch fails to compile
#include "oet.h"

OET_ENUM(Stage,
         Decode,
         Execute,
         Writeback);

struct Instruction : oet::Object {
  oet::Const<oet::UInt<8>> pc;
  oet::Const<bool> odd;
  oet::Mutable<Stage> stage;
  oet::SetOnce<oet::UInt<8>> result;
};
OET_OBJECT(Instruction, "instruction", pc, odd, stage, result)

struct Request : oet::Object {
  oet::Parent<Instruction> insn;  // the instruction that issued it
};
OET_OBJECT(Request, "request", insn)

struct SampleValues { oet::UInt<1> pass; };
OET_FIELDS(SampleValues, pass)
struct IdleValues { oet::UInt<8> count; };
OET_FIELDS(IdleValues, count)

oet::Collection<Instruction> active{"active"};
oet::Collection<Request> requests{"requests"};

oet::Event<Instruction> decode{"decode", oet::create, &Instruction::pc, &Instruction::odd, &Instruction::stage};
oet::Event<Instruction> issue{"issue", &Instruction::stage};
oet::Event<Instruction> writeback{"writeback", &Instruction::stage, &Instruction::result};
oet::Event<Instruction> retire{"retire", oet::complete};
oet::Event<Instruction> squash{"squash", oet::abort, oet::by<Instruction>()};
oet::Event<Instruction> stall{"stall", oet::by<Instruction>()};
oet::Event<Instruction, SampleValues> sample{"sample"};
oet::Event<Instruction> second{"second"};
oet::Event<Instruction> youngest{"youngest"};
oet::Event<Instruction> busy{"busy"};
oet::Event<Instruction> crowd{"crowd"};
oet::Event<oet::none, IdleValues> idle{"idle"};
oet::Event<Request> request_issue{"issue", oet::create, &Request::insn};
oet::Event<Request> request_done{"done", oet::complete};
oet::Event<Instruction> pick{"pick"};

oet::Call decode_call{"decode", {decode}};
oet::Call issue_call{"issue", {issue}};
oet::Call writeback_call{"writeback", {writeback}};
oet::Call retire_call{"retire", {retire}};
oet::Call squash_call{"squash", {squash}};
oet::Call stall_call{"stall", {stall}};
oet::Call sample_first_call{"sample_first", {sample}};    // pass 0
oet::Call sample_second_call{"sample_second", {sample}};  // pass 1
oet::Call second_call{"second", {second, youngest}};
oet::Call every3_call{"every3", {busy, crowd, idle}};
oet::Call request_issue_call{"request_issue", {request_issue}};
oet::Call request_done_call{"request_done", {request_done}};
oet::Call pick_call{"pick", {pick}};

Instruction& in(Stage s) { return active.where([s](Instruction& m) { return m.stage == s; }).only(); }

// Join order is admission order.
void oet_decode(long long cycle, unsigned char pc) {
  oet::at(cycle, decode_call, [=] {
    Instruction& i = decode.create();
    i.pc = pc;
    i.odd = (pc & 1) != 0;
    i.stage = Stage::Decode;
    active.add(i);
    decode.emit(i);
  });
}
// Found by its stage.
void oet_issue(long long cycle) {
  oet::at(cycle, issue_call, [] {
    Instruction& i = in(Stage::Decode);
    i.stage = Stage::Execute;
    issue.emit(i);
  });
}
// Found by a hardware key.
void oet_writeback(long long cycle, unsigned char pc, unsigned char result) {
  oet::at(cycle, writeback_call, [=] {
    Instruction& i = active.where([=](Instruction& m) { return m.pc == pc; }).only();
    i.stage = Stage::Writeback;
    i.result = result;
    writeback.emit(i);
  });
}
// The oldest instruction retires.
void oet_retire(long long cycle) {
  oet::at(cycle, retire_call, [] {
    Instruction& i = active.head();
    active.remove(i);
    retire.emit(i);
  });
}
// The instruction in execute squashes every younger one.
void oet_squash(long long cycle) {
  oet::at(cycle, squash_call, [] {
    Instruction& br = in(Stage::Execute);
    for (Instruction& i : active.where([&](Instruction& m) { return m.id > br.id; })) {
      active.remove(i);
      squash.emit(i, oet::by(br));
    }
  });
}
// The instruction in decode waits on the one in execute.
void oet_stall(long long cycle) {
  oet::at(cycle, stall_call, [] { stall.emit(in(Stage::Decode), oet::blocked(in(Stage::Execute))); });
}
// Two events per member, even PCs before odd ones and otherwise in join order.
void oet_sample(long long cycle) {
  const auto by_odd = [](Instruction& m) { return m.odd.get(); };
  oet::at(cycle, sample_first_call, [=] {
    for (Instruction& i : active.order_by(by_odd)) sample.emit(i, {.pass = 0});
  });
  oet::at(cycle, sample_second_call, [=] {
    for (Instruction& i : active.order_by(by_odd)) sample.emit(i, {.pass = 1});
  });
}
// A position in join order, and the head of a ranking by stage.
void oet_second(long long cycle) {
  oet::at(cycle, second_call, [] {
    second.emit(active.at(1));
    youngest.emit(active.order_by([](Instruction& m) { return m.stage.get(); }).head());
  });
}
// Tests on a view's size.
void oet_every3(long long cycle, unsigned char exe_pc, unsigned char count) {
  oet::at(cycle, every3_call, [=] {
    auto executing = active.where([=](Instruction& m) { return m.pc == exe_pc && m.stage == Stage::Execute; });
    if (executing.size() == 1) {
      busy.emit(executing.only());
      if (active.size() >= 3)
        for (Instruction& j : active) crowd.emit(j);
    } else {
      idle.emit({.count = count});
    }
  });
}
// A request keeps the ID of the instruction in execute that issued it.
void oet_request_issue(long long cycle) {
  oet::at(cycle, request_issue_call, [] {
    Instruction& by = in(Stage::Execute);
    Request& r = request_issue.create();
    r.insn = by;
    requests.add(r);
    request_issue.emit(r);
  });
}
void oet_request_done(long long cycle) {
  oet::at(cycle, request_done_call, [] {
    Request& r = requests.head();
    requests.remove(r);
    request_done.emit(r);
  });
}
// A hardware bit as a term of the query: with it set, the oldest instruction, else the one in decode.
void oet_pick(long long cycle, svBit oldest) {
  oet::at(cycle, pick_call, [=] {
    pick.emit(active.where([=](Instruction& m) { return m.stage == Stage::Decode || oldest; }).head());
  });
}
