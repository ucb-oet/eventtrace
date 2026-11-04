// Drives a small pipeline through the C++ library without a simulator, as a SystemVerilog design's DPI functions
// would, and checks the trace by reading it back with the reader, and the manifest line by line. Each broken rule
// runs in a child process, which must stop with its message.
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "oet.h"

extern "C" void oet_open(const char* path, const char* start, const char* stop);
extern "C" void oet_finish();

OET_ENUM(Stage,
         Decode,      // waiting in decode
         Execute,
         Writeback);

OET_ENUM(TrapCause,
         Misaligned = 2,
         Illegal = 5);

struct Instruction : oet::Object {
  oet::Const<oet::UInt<16>> pc;
  oet::Mutable<Stage> stage;
  oet::SetOnce<oet::SInt<8>> result;
};
OET_OBJECT(Instruction, "instruction", pc, stage, result)

struct Request : oet::Object {
  oet::Parent<Instruction> insn;  // the instruction that issued it
  oet::Const<bool> write;
};
OET_OBJECT(Request, "request", insn, write)

struct TrapValues {
  TrapCause cause;
  std::optional<oet::UInt<4>> level;
};
OET_FIELDS(TrapValues, cause, level)

struct TickValues {
  oet::UInt<16> count;
  oet::SInt<8> delta;
  oet::UInt<64> stamp;
};
OET_FIELDS(TickValues, count, delta, stamp)

oet::Collection<Instruction> active{"active"};
oet::Collection<Request> requests{"requests"};

oet::Event<Instruction> decode{"decode", oet::create, &Instruction::pc, &Instruction::stage, oet::by<Instruction>()};
oet::Event<Instruction> issue{"issue", &Instruction::stage};
oet::Event<Instruction> writeback{"writeback", &Instruction::stage, oet::optional(&Instruction::result)};
oet::Event<Instruction> retire{"retire", oet::complete};
oet::Event<Instruction> squash{"squash", oet::abort, oet::by<Instruction>()};
oet::Event<Instruction, TrapValues> trap{"trap"};
oet::Event<Request> request{"issue", oet::create, &Request::insn, &Request::write};
oet::Event<oet::none, TickValues> tick{"tick"};

// Sites: decode[0] 0-1, decode[1] 2-3, issue 4-5, writeback 6-7, retire 8-9, squash 10-11, trap 12-13, request
// 14-15, tick 16.
oet::Call decode_call{"decode", 2, {decode}};
oet::Call issue_call{"issue", {issue}};
oet::Call writeback_call{"writeback", {writeback}};
oet::Call end_call{"end", {retire, squash, trap}};
oet::Call request_call{"request", {request}};
oet::Call tick_call{"tick", {tick}};

Instruction& in(Stage s) {
  return active.where([s](Instruction& m) { return m.stage == s; }).order_by([](Instruction& m) { return m.pc.get(); }).head();
}

namespace {

int failures = 0;

void expect(bool ok, const std::string& what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// The DPI functions of the pipeline, as a design's C++ would write them.
void dpi_decode(long long cycle, int slot, unsigned pc, bool blocked) {
  oet::at(cycle, decode_call[slot], [=] {
    Instruction& i = decode.create();
    i.pc = pc;
    i.stage = Stage::Decode;
    active.add(i);
    if (blocked) decode.emit(i, oet::blocked(in(Stage::Decode)));
    else decode.emit(i);
  });
}
void dpi_issue(long long cycle) {
  oet::at(cycle, issue_call, [] {
    Instruction& i = in(Stage::Decode);
    i.stage = Stage::Execute;
    issue.emit(i);
  });
}
void dpi_writeback(long long cycle, int result) {
  oet::at(cycle, writeback_call, [=] {
    Instruction& i = in(Stage::Execute);
    i.stage = Stage::Writeback;
    if (result != 0) i.result = result;
    writeback.emit(i);
  });
}
// Retires the instruction in Writeback; with `kill`, squashes every younger one, and with `cause`, traps the oldest
// in Decode.
void dpi_end(long long cycle, bool kill, unsigned cause) {
  oet::at(cycle, end_call, [=] {
    Instruction& done = in(Stage::Writeback);
    if (cause) trap.emit(in(Stage::Decode), {.cause = TrapCause(cause)});
    if (kill)
      for (Instruction& i : active.where([&](Instruction& m) { return m.pc > done.pc.get(); })) {
        active.remove(i);
        squash.emit(i, oet::by(done));
      }
    active.remove(done);
    retire.emit(done);
  });
}
void dpi_request(long long cycle, bool write) {
  oet::at(cycle, request_call, [=] {
    Request& r = request.create();
    r.insn = in(Stage::Execute);
    r.write = write;
    requests.add(r);
    request.emit(r);
  });
}
void dpi_tick(long long cycle, unsigned count, int delta, unsigned long long stamp) {
  oet::at(cycle, tick_call, [=] { tick.emit({.count = count, .delta = delta, .stamp = stamp}); });
}

// Runs `body` in a child process, which must stop with a message containing `error`.
template <class F>
void expect_violation(const char* label, const char* error, F body) {
  int err[2];
  if (pipe(err) != 0) std::abort();
  const pid_t child = fork();
  if (child == 0) {
    dup2(err[1], 2);
    oet_open((std::string("build/oet_violation_") + label + ".trace").c_str(), "", "");
    body();
    oet_finish();
    std::_Exit(0);
  }
  close(err[1]);
  std::string message;
  char buffer[512];
  for (ssize_t n; (n = read(err[0], buffer, sizeof buffer)) > 0;) message.append(buffer, static_cast<std::size_t>(n));
  close(err[0]);
  int status = 0;
  waitpid(child, &status, 0);
  if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT || message.find(error) == std::string::npos) {
    std::printf("FAIL: %s should stop with \"%s\", but printed \"%s\"\n", label, error, message.c_str());
    ++failures;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: oet_test <eventtrace reader>\n");
    return 2;
  }

  // Without a trace open, calls do nothing.
  dpi_tick(0, 1, 1, 0);

  // Broken rules, each in a child.
  expect_violation("no_member", "issue at site 4 (call issue, cycle 0): head found no members of active.where(...)",
                   [] { dpi_issue(0); });
  expect_violation("const_twice", "instruction#0.pc is const, so only its create event assigns it", [] {
    dpi_decode(0, 0, 0x10, false);
    oet::at(1, issue_call, [] {
      Instruction& i = in(Stage::Decode);
      i.pc = 7;
    });
  });
  expect_violation("design_check", "issue at site 4 (call issue, cycle 0): the design's own check failed", [] {
    oet::at(0, issue_call, [] { oet::fail("the design's own check failed"); });
  });
  expect_violation("pending", "instruction#0 has effects that no event emitted on it", [] {
    dpi_decode(0, 0, 0x10, false);
    oet::at(1, issue_call, [] { in(Stage::Decode).stage = Stage::Execute; });
  });
  expect_violation("undeclared", "instruction.retire is emitted, but call issue does not declare it", [] {
    dpi_decode(0, 0, 0x10, false);
    oet::at(1, issue_call, [] {
      Instruction& i = in(Stage::Decode);
      active.remove(i);
      retire.emit(i);
    });
  });
  expect_violation("required_value", "instruction.trap has the value cause, which this record does not set", [] {
    dpi_decode(0, 0, 0x10, false);
    oet::at(1, end_call, [] { trap.emit(in(Stage::Decode), {.level = 3}); });
  });
  expect_violation("too_wide", "pc = 65536 does not fit UInt<16>", [] { dpi_decode(0, 0, 0x10000, false); });
  expect_violation("enum_wide", "cause = 9 does not fit TrapCause", [] {
    dpi_decode(0, 0, 0x10, false);
    oet::at(1, end_call, [] { trap.emit(in(Stage::Decode), {.cause = TrapCause(9)}); });
  });
  expect_violation("unlisted", "instruction#0.result is assigned, but instruction.issue does not list it", [] {
    dpi_decode(0, 0, 0x10, false);
    oet::at(1, issue_call, [] {
      Instruction& i = in(Stage::Decode);
      i.stage = Stage::Execute;
      i.result = 1;
      issue.emit(i);
    });
  });
  expect_violation("listed", "instruction.issue lists the field stage, which this record does not set", [] {
    dpi_decode(0, 0, 0x10, false);
    oet::at(1, issue_call, [] { issue.emit(in(Stage::Decode)); });
  });
  expect_violation("blocked_abort", "instruction.squash aborts its subject, so a record of it is not blocked", [] {
    dpi_decode(0, 0, 0x10, false);
    oet::at(1, end_call, [] {
      Instruction& i = in(Stage::Decode);
      active.remove(i);
      squash.emit(i, oet::blocked());
    });
  });
  expect_violation("twice", "call decode[1] fires twice in cycle 4", [] {
    dpi_decode(4, 1, 0x10, false);
    dpi_decode(4, 1, 0x14, false);
  });
  expect_violation("index", "call decode has 2 indices, but is given index 2", [] { dpi_decode(0, 2, 0x10, false); });
  expect_violation("set_once", "instruction#0.result is setOnce and was assigned in an earlier cycle", [] {
    dpi_decode(0, 0, 0x10, false);
    dpi_issue(1);
    dpi_writeback(2, 5);
    oet::at(3, writeback_call, [] {
      Instruction& i = in(Stage::Writeback);
      i.stage = Stage::Writeback;
      i.result = 6;
      writeback.emit(i);
    });
  });
  expect_violation("still_member", "instruction#0 is still in active after its destroy", [] {
    dpi_decode(0, 0, 0x10, false);
    dpi_issue(1);
    dpi_writeback(2, 0);
    oet::at(3, end_call, [] { retire.emit(in(Stage::Writeback)); });
  });
  expect_violation("unranked", "instruction#0 has no key to rank by in active", [] {
    dpi_decode(0, 0, 0x10, false);
    oet::at(1, issue_call, [] { active.order_by([](Instruction& m) { return m.result.get(); }).head(); });
  });
  expect_violation("created_now", "instruction.issue is emitted on instruction#0, which was created in this cycle", [] {
    oet::at(0, decode_call[0], [] {
      Instruction& i = decode.create();
      i.pc = 1;
      i.stage = Stage::Decode;
      decode.emit(i);
      i.stage = Stage::Execute;
      issue.emit(i);
    });
  });

  // The pipeline, with A to D at pc 0x100, 0x104, 0x108, and 0x10c. Cycle 0 decodes A and B. Cycle 1 issues A, the
  // oldest in Decode, and decodes C, blocked by A. Cycle 2 writes A back and ticks. Cycle 3 retires A, squashes the
  // younger B and C, and traps B, the oldest in Decode. Cycle 4 decodes D, 5 issues it, and 6 issues a request from
  // it and ticks.
  oet_open("build/oet_test.trace", "", "");
  dpi_decode(0, 0, 0x100, false);
  dpi_decode(0, 1, 0x104, false);
  dpi_issue(1);
  dpi_decode(1, 0, 0x108, true);
  dpi_writeback(2, -3);
  dpi_tick(2, 7, -2, ~0ull);
  dpi_end(3, true, 5);
  dpi_decode(4, 1, 0x10c, false);
  dpi_issue(5);
  dpi_request(6, true);
  dpi_tick(6, 9, 3, 6);
  oet_finish();

  const std::string command = std::string(argv[1]) + " dump build/oet_test.trace.json build/oet_test.trace";
  std::FILE* pipe = popen(command.c_str(), "r");
  std::string dump;
  char buffer[4096];
  for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0;) dump.append(buffer, n);
  expect(pclose(pipe) == 0, "the reader reads the trace with its manifest");
  // Each cycle's records are in site order, whatever order the handler emitted them in. The blocked decode takes the
  // blocking site after its slot's, 1. Object IDs count per type from 0; the reader numbers events in trace order.
  const std::string want =
      "eventtrace cycle=0 event_id=0 site_id=0 instruction.decode subject=instruction#0 pc=256 stage=Decode\n"
      "eventtrace cycle=0 event_id=1 site_id=2 instruction.decode subject=instruction#1 pc=260 stage=Decode\n"
      "eventtrace cycle=1 event_id=2 site_id=1 instruction.decode subject=instruction#2 by=instruction#0 pc=264 stage=Decode\n"
      "eventtrace cycle=1 event_id=3 site_id=4 instruction.issue subject=instruction#0 stage=Execute\n"
      "eventtrace cycle=2 event_id=4 site_id=6 instruction.writeback subject=instruction#0 result=-3 stage=Writeback\n"
      "eventtrace cycle=2 event_id=5 site_id=16 tick count=7 delta=-2 stamp=0xffffffffffffffff\n"
      "eventtrace cycle=3 event_id=6 site_id=8 instruction.retire subject=instruction#0\n"
      "eventtrace cycle=3 event_id=7 site_id=10 instruction.squash subject=instruction#1 by=instruction#0\n"
      "eventtrace cycle=3 event_id=8 site_id=10 instruction.squash subject=instruction#2 by=instruction#0\n"
      "eventtrace cycle=3 event_id=9 site_id=12 instruction.trap subject=instruction#1 cause=Illegal\n"
      "eventtrace cycle=4 event_id=10 site_id=2 instruction.decode subject=instruction#3 pc=268 stage=Decode\n"
      "eventtrace cycle=5 event_id=11 site_id=4 instruction.issue subject=instruction#3 stage=Execute\n"
      "eventtrace cycle=6 event_id=12 site_id=14 request.issue subject=request#0 insn=instruction#3 write=1\n"
      "eventtrace cycle=6 event_id=13 site_id=16 tick count=9 delta=3 stamp=0x0000000000000006\n";
  expect(dump == want, "the trace reads back as the pipeline's 14 records:\n" + dump);

  // The manifest, in ManifestGen's layout; some of its lines.
  const std::string manifest = slurp("build/oet_test.trace.json");
  for (const char* line : {
           "        {\"name\": \"by\", \"type\": \"Id\", \"object\": \"instruction\", \"optional\": true, \"role\": \"blocked_by\"},\n",
           "        {\"name\": \"by\", \"type\": \"Id\", \"object\": \"instruction\", \"optional\": true, \"role\": \"aborted_by\"}\n",
           "        {\"name\": \"level\", \"type\": \"UInt\", \"width\": 4, \"optional\": true}\n",
           "        {\"name\": \"stamp\", \"type\": \"UInt\", \"width\": 64}\n",
           "        {\"name\": \"cause\", \"type\": \"Enum\", \"enum\": \"TrapCause\", \"width\": 3},\n",
           "        {\"name\": \"result\", \"type\": \"SInt\", \"width\": 8, \"property\": \"setOnce\"}\n",
           "        {\"name\": \"insn\", \"type\": \"Id\", \"object\": \"instruction\", \"property\": \"const\", \"role\": \"parent\"},\n",
           "    {\"name\": \"TrapCause\", \"values\": {\"Misaligned\": 2, \"Illegal\": 5}}\n",
           "    {\"site_id\": 3, \"event_name\": \"instruction.decode\", \"blocks\": true},\n",
           "    {\"site_id\": 16, \"event_name\": \"tick\"}\n"})
    expect(manifest.find(line) != std::string::npos, std::string("the manifest has the line ") + line);
  expect(manifest.find("\"instance\"") == std::string::npos && manifest.find("\"source\"") == std::string::npos,
         "the manifest's sites have no instance or source");

  if (failures) return 1;
  std::printf("PASS: calls without a trace open do nothing; 17 broken rules stop the run with their messages; the "
              "pipeline's records are written in site order with blocking sites, causes, values up to 64 bits wide, a "
              "parent, and a subjectless event, and the reader reads them with the manifest the library wrote beside the trace\n");
  return 0;
}
