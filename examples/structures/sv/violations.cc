// Drives Misuse.scala's sites, as the C++ library's calls, with synthetic calls rather than a simulation, as
// violations.cc drives the Chisel build's generated handlers: the same scenarios, each in a child process, which
// must stop with its expected violation or, for the passing scenarios, finish normally with its expected records.
// Messages differ from the Chisel build's where the flows do: the context names the call, and sites have the C++
// library's numbers. One more scenario covers the library's own rule for clear: each member it returns must have an
// event emitted on it.
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "oet.h"

// The library's entry point that `OET_SETUP imports to open the trace at `path`, with the recording window's
// triggers, and write the manifest beside it as <path>.json; called here directly, without a simulator.
extern "C" void oet_open(const char* path, const char* start, const char* stop);
// The library's entry point that `OET_SETUP imports to finish the trace, handling the last cycle's calls.
extern "C" void oet_finish();

// An object held by the fixture's queue, ROB, and map.
struct Cell : oet::Object {
  oet::Const<oet::UInt<4>> key;
};
OET_OBJECT(Cell, "cell", key)

oet::Queue<Cell> q{"Misuse.q", 2};
oet::Rob<Cell> r{"Misuse.r", 2, 4};
oet::Map<Cell> m{"Misuse.m", 2};

oet::Event<Cell> enq0{"enq0", oet::create, &Cell::key};
oet::Event<Cell> enq1{"enq1", oet::create, &Cell::key};
oet::Event<Cell> enq2{"enq2", oet::create, &Cell::key};
oet::Event<Cell> deq{"deq", oet::complete};
oet::Event<Cell> deq1{"deq1", oet::complete};
oet::Event<Cell> flush{"flush", oet::abort};
oet::Event<Cell> alloc0{"alloc0", oet::create, &Cell::key};
oet::Event<Cell> alloc1{"alloc1", oet::create, &Cell::key};
oet::Event<Cell> alloc2{"alloc2", oet::create, &Cell::key};
oet::Event<Cell> alloc3{"alloc3", oet::create, &Cell::key};
oet::Event<Cell> find1{"find1"};
oet::Event<Cell> squash1{"squash1", oet::abort};
oet::Event<Cell> squash3{"squash3", oet::abort};
oet::Event<Cell> commit{"commit", oet::complete};
oet::Event<Cell> put_a{"put_a", oet::create, &Cell::key};
oet::Event<Cell> put_b{"put_b", oet::create, &Cell::key};
oet::Event<Cell> take{"take", oet::complete};
oet::Event<Cell> find0{"find0"};
oet::Event<Cell> deq_keep{"deq_keep"};
oet::Event<Cell> drain{"drain"};
oet::Event<Cell> forget{"forget"};
oet::Event<Cell> trim1{"trim1", oet::abort};
oet::Event<Cell> trim3{"trim3", oet::abort};

namespace {

// One call per Chisel site, in its order, each emitting its site's event; its handler by name.
std::map<std::string, std::pair<oet::Call*, std::function<void()>>> calls;

// Declares the call `name`, which emits `event` and runs `handler`.
void call(oet::detail::EventBase& event, const char* name, std::function<void()> handler) {
  calls[name] = {new oet::Call(name, {event}), std::move(handler)};
}

// A create handler for `event`: a cell with key `k`, which `join` adds to a collection.
std::function<void()> make(oet::Event<Cell>& event, unsigned k, std::function<void(Cell&)> join) {
  return [&event, k, join] {
    Cell& c = event.create();
    c.key = k;
    join(c);
    event.emit(c);
  };
}

// Declares every call, in Misuse.scala's site order.
void declare() {
  call(enq0, "enq0", make(enq0, 0, [](Cell& c) { q.add(c); }));
  call(enq1, "enq1", make(enq1, 1, [](Cell& c) { q.add(c); }));
  call(enq2, "enq2", make(enq2, 2, [](Cell& c) { q.add(c); }));
  call(deq, "deq", [] { deq.emit(q.pop()); });
  call(deq1, "deq1", [] { deq1.emit(q.pop(1)); });
  call(flush, "flush", [] { for (Cell& c : q.clear()) flush.emit(c); });
  call(alloc0, "alloc0", make(alloc0, 0, [](Cell& c) { r.add(0, c); }));
  call(alloc1, "alloc1", make(alloc1, 1, [](Cell& c) { r.add(1, c); }));
  call(alloc2, "alloc2", make(alloc2, 2, [](Cell& c) { r.add(2, c); }));
  call(alloc3, "alloc3", make(alloc3, 3, [](Cell& c) { r.add(3, c); }));
  call(find1, "find1", [] { find1.emit(r(1)); });
  call(squash1, "squash1", [] { for (Cell& c : r.rollback(1)) squash1.emit(c); });
  call(squash3, "squash3", [] { for (Cell& c : r.rollback(3)) squash3.emit(c); });
  call(commit, "commit", [] { commit.emit(r.pop()); });
  call(put_a, "put_a", make(put_a, 0, [](Cell& c) { m.add(1, c); }));
  call(put_b, "put_b", make(put_b, 1, [](Cell& c) { m.add(1, c); }));
  call(take, "take", [] {
    Cell& x = m(1);
    m.remove(x);
    take.emit(x);
  });
  call(find0, "find0", [] { find0.emit(m(0)); });
  // A pop and a clear that keep the member alive, so that the only rule they break is removing it twice.
  call(deq_keep, "deq_keep", [] { deq_keep.emit(q.pop()); });
  call(drain, "drain", [] { for (Cell& c : q.clear()) drain.emit(c); });
  // A clear whose members get no event, so nothing carries their removal.
  call(forget, "forget", [] { q.clear(); });
  // The youngest one or three members leave.
  call(trim1, "trim1", [] { for (Cell& c : q.drop_youngest(1)) trim1.emit(c); });
  call(trim3, "trim3", [] { for (Cell& c : q.drop_youngest(3)) trim3.emit(c); });
}

// A sequence of synthetic calls and how its run must end: stopped with a violation containing every part of
// `violation`, or, if that is empty, finished normally with a trace containing `records`.
struct Scenario {
  const char* label;
  std::vector<std::vector<std::string>> cycles;  // the calls firing in cycles 0, 1, ...
  std::vector<const char*> violation;            // parts of the expected violation, or none for a passing run
  const char* records;                           // for a passing run, part of its expected records
};

std::string dump, trace_dir;

// Everything readable from `fd`, which it then closes.
std::string drain_fd(int fd) {
  std::string s;
  char buffer[4096];
  for (ssize_t n; (n = read(fd, buffer, sizeof buffer)) > 0;) s.append(buffer, static_cast<std::size_t>(n));
  close(fd);
  return s;
}

// The trace at `path` as text, with the manifest the library wrote beside it.
std::string text_of(const std::string& path) {
  const std::string command = dump + " " + path + ".json " + path;
  std::FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) std::abort();
  std::string text;
  char buffer[4096];
  for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0;) text.append(buffer, n);
  pclose(pipe);
  return text;
}

// Runs scenario `s` in a child process and reports whether it ended as expected.
bool run(const Scenario& s, std::size_t index) {
  const std::string trace = trace_dir + "/sv_violation" + std::to_string(index) + ".trace";
  std::fflush(stdout);  // or the child would print what the parent has buffered
  int err[2];
  if (pipe(err) != 0) std::abort();
  const pid_t pid = fork();
  if (pid == 0) {
    dup2(err[1], 2);
    oet_open(trace.c_str(), "", "");
    for (std::size_t cycle = 0; cycle < s.cycles.size(); ++cycle)
      for (const std::string& name : s.cycles[cycle]) {
        const auto& [c, handler] = calls.at(name);
        oet::at(static_cast<long long>(cycle), *c, handler);
      }
    oet_finish();
    std::exit(0);
  }
  close(err[1]);
  const std::string errors = drain_fd(err[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  const bool aborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
  const bool finished = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  const std::string records = finished ? text_of(trace) : std::string();
  bool ok;
  if (!s.violation.empty()) {
    ok = aborted;
    for (const char* part : s.violation) ok = ok && errors.find(part) != std::string::npos;
  } else {
    ok = finished && errors.empty() && records.find(s.records) != std::string::npos;
  }
  const std::string shown = s.violation.empty() ? std::string("finished normally") : errors.substr(0, errors.find('\n'));
  std::printf("%s: %s: %s\n", ok ? "PASS" : "FAIL", s.label, ok ? shown.c_str() : (errors + records).c_str());
  return ok;
}

}  // namespace

// Takes the dump command and the trace directory, declares the calls, then runs every scenario and fails if any did
// not end as expected.
int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <dump command> <trace directory>\n", argv[0]);
    return 2;
  }
  dump = argv[1];
  trace_dir = argv[2];
  declare();

  const std::vector<Scenario> scenarios = {
    {"a queue's lifetime", {{"enq0"}, {"enq1"}, {"deq"}, {"deq"}}, {}, "cell.deq subject=cell#1"},
    {"a queue cleared", {{"enq0"}, {"enq1"}, {"flush"}, {"enq0"}}, {}, "cell.flush subject=cell#1"},
    {"two pops in one cycle", {{"enq0"}, {"enq1"}, {"deq", "deq1"}}, {}, "cell.deq1 subject=cell#1"},
    {"the youngest member dropped", {{"enq0"}, {"enq1"}, {"trim1"}, {"deq"}}, {}, "cell.deq subject=cell#0"},
    {"a pop and a drop in one cycle", {{"enq0"}, {"enq1"}, {"deq", "trim1"}}, {}, "cell.trim1 subject=cell#1"},
    {"a ROB's keys wrapping", {{"alloc0"}, {"alloc1"}, {"commit"}, {"alloc2"}, {"commit"}, {"alloc3"}, {"commit"},
     {"alloc0"}}, {}, "cell.alloc0 subject=cell#4 key=0"},
    {"a rollback moving the tail back", {{"alloc0"}, {"alloc1"}, {"squash1"}, {"alloc1"}}, {},
     "cell.alloc1 subject=cell#2 key=1"},
    {"a rollback to the tail itself", {{"alloc0"}, {"squash1"}, {"alloc1"}}, {}, "cell.alloc1 subject=cell#1 key=1"},
    {"a map key freed and taken in one cycle", {{"put_a"}, {"take", "put_b"}}, {}, "cell.put_b subject=cell#1 key=1"},
    {"a pop past the cycle's other pops", {{"enq0"}, {"enq1"}, {"deq1"}},
     {"cell#1 is popped from Misuse.q at position 1, but this cycle's 1 pop must take its first member"}, nullptr},
    {"a queue past its entries", {{"enq0"}, {"enq1"}, {"enq2"}},
     {"Misuse.q holds 3 members at the end of the cycle, more than its 2 entries"}, nullptr},
    {"a pop and a clear taking one member", {{"enq0"}, {"deq_keep", "drain"}},
     {"cell#0 is removed from Misuse.q twice in one cycle"}, nullptr},
    {"dropping more members than there are", {{"enq0"}, {"enq1"}, {"trim3"}},
     {"drop_youngest(3) found only 2 members of Misuse.q"}, nullptr},
    {"a ROB key out of sequence", {{"alloc0"}, {"alloc2"}},
     {"cell#1 joins Misuse.r with key 2, but its tail allocates 1 next"}, nullptr},
    {"a ROB key without a member", {{"alloc0"}, {"find1"}},
     {"eventtrace: cell.find1 at site ", " (call find1, cycle 1): Misuse.r has no member with key 1"}, nullptr},
    {"a rollback to a key neither live nor the tail", {{"alloc0"}, {"squash3"}},
     {"rolls Misuse.r back to key 3, which is neither a live key nor its tail, 1"}, nullptr},
    {"a ROB past its entries", {{"alloc0"}, {"alloc1"}, {"alloc2"}},
     {"Misuse.r holds 3 members at the end of the cycle, more than its 2 entries"}, nullptr},
    {"a map key already held", {{"put_a"}, {"put_b"}}, {"cell#1 joins Misuse.m with key 1, which cell#0 holds"},
     nullptr},
    {"a map key without a member", {{"find0"}}, {"Misuse.m has no member with key 0"}, nullptr},
    {"a cleared member without an event", {{"enq0"}, {"forget"}}, {"cell#0 has effects that no event emitted on it"},
     nullptr},
  };

  int failures = 0;
  for (std::size_t i = 0; i < scenarios.size(); ++i) failures += !run(scenarios[i], i);
  if (failures) {
    std::printf("FAIL: %d of %zu scenarios\n", failures, scenarios.size());
    return 1;
  }
  std::printf("PASS: all %zu scenarios: each violation of a queue, ROB, or map stops the C++ library with its call, "
              "cycle, and objects\n", scenarios.size());
  return 0;
}
