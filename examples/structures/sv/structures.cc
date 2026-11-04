// Structures' C++: Structures.scala's trace as calls, one per `when` block in its order, each handler with that
// block's queries and effects on the fetch queue, the ROB, and the reservation station.
#include "VTestTop__Dpi.h"  // Verilator's prototypes of the design's imports, so a mismatch fails to compile
#include "oet.h"

// An instruction, from fetch until it commits or is squashed.
struct Insn : oet::Object {
  oet::Const<oet::UInt<8>> pc;
};
OET_OBJECT(Insn, "insn", pc)

// An instruction's operation while it waits in the reservation station.
struct Uop : oet::Object {
  oet::Const<oet::UInt<8>> pc;
  oet::Parent<Insn> insn;  // the instruction it carries out
};
OET_OBJECT(Uop, "uop", pc, insn)

// complete's value: whether the instruction is a branch that mispredicted.
struct CompleteValues { oet::Bool mispredict; };
OET_FIELDS(CompleteValues, mispredict)

oet::Queue<Insn> fetch_queue{"fetchQueue", 4};
oet::Rob<Insn> rob{"rob", 8, 16};
oet::Map<Uop> station{"station", 4};

oet::Event<Insn> insn_fetch{"fetch", oet::create, &Insn::pc};
oet::Event<Insn> insn_dispatch{"dispatch"};
oet::Event<Uop> uop_dispatch{"dispatch", oet::create, &Uop::pc, &Uop::insn};
oet::Event<Uop> uop_execute{"execute", oet::complete};
oet::Event<Insn, CompleteValues> insn_complete{"complete"};
oet::Event<Insn> insn_squash{"squash", oet::abort};
oet::Event<Uop> uop_squash{"squash", oet::abort};
oet::Event<Insn> insn_flush{"flush", oet::abort};
oet::Event<Insn> insn_commit{"commit", oet::complete};

oet::Call fetch_call{"fetch", {insn_fetch}};
oet::Call dispatch_call{"dispatch", {insn_dispatch, uop_dispatch}};
oet::Call jump_call{"jump", {insn_flush}};
oet::Call execute_call{"execute", {uop_execute, insn_complete}};
oet::Call mispredict_call{"mispredict", {insn_squash, uop_squash, insn_flush}};
oet::Call commit_call{"commit", {insn_commit}};

// The next PC joins the fetch queue's tail.
void oet_fetch(long long cycle, unsigned char pc) {
  oet::at(cycle, fetch_call, [=] {
    Insn& i = insn_fetch.create();
    i.pc = pc;
    fetch_queue.add(i);
    insn_fetch.emit(i);
  });
}

// The fetch queue's head moves into the ROB under the hardware's tag, and its operation into a free slot.
void oet_dispatch(long long cycle, unsigned char rob_tail, unsigned char pc, unsigned char slot) {
  oet::at(cycle, dispatch_call, [=] {
    Insn& i = fetch_queue.pop();
    rob.add(rob_tail, i);
    insn_dispatch.emit(i);
    Uop& u = uop_dispatch.create();
    u.pc = pc;
    u.insn = i;
    station.add(slot, u);
    uop_dispatch.emit(u);
  });
}

// A jump that dispatch resolves: the fetch queue's `younger` entries, fetched after it, leave.
void oet_jump(long long cycle, unsigned char younger) {
  oet::at(cycle, jump_call, [=] {
    for (Insn& i : fetch_queue.drop_youngest(younger)) insn_flush.emit(i);
  });
}

// The operation in `slot` leaves the station, and its instruction, found by its tag, completes.
void oet_execute(long long cycle, unsigned char slot, unsigned char tag, svBit mispredict) {
  oet::at(cycle, execute_call, [=] {
    Uop& u = station(slot);
    station.remove(u);
    uop_execute.emit(u);
    insn_complete.emit(rob(tag), {.mispredict = mispredict != 0});
  });
}

// The ROB's tail moves back to `cut`, just after the branch; younger operations leave the station; and the fetch
// queue empties.
void oet_mispredict(long long cycle, unsigned char cut, unsigned char exe_pc) {
  oet::at(cycle, mispredict_call, [=] {
    for (Insn& i : rob.rollback(cut)) insn_squash.emit(i);
    for (Uop& u : station.where([=](Uop& m) { return m.pc > exe_pc; })) {
      station.remove(u);
      uop_squash.emit(u);
    }
    for (Insn& i : fetch_queue.clear()) insn_flush.emit(i);
  });
}

// The ROB's head commits.
void oet_commit(long long cycle) {
  oet::at(cycle, commit_call, [] { insn_commit.emit(rob.pop()); });
}
