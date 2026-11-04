// Drives Misuse.scala's sites, as the C++ library's calls, with synthetic calls rather than a simulation, as
// violations.cc drives the Chisel build's generated handlers: the same scenarios, each in a child process, which
// must stop with its expected violation or, for the passing scenarios, finish normally with its expected records.
// Messages differ from the Chisel build's where the flows do: a query names its view without the predicate's text,
// the context names the call, and sites have the C++ library's numbers.
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

// The object Misuse.scala's collections hold, with a field of each property: const, setOnce, and mutable.
struct Cell : oet::Object {
  oet::Const<oet::UInt<4>> key;
  oet::SetOnce<oet::UInt<4>> once;
  oet::Mutable<oet::UInt<4>> value;
};
OET_OBJECT(Cell, "cell", key, once, value)

// An object type that no collection holds.
struct Note : oet::Object {
  oet::Const<oet::UInt<4>> v;
};
OET_OBJECT(Note, "note", v)

// census's value: `first`, the ID of a's head.
struct CensusValues { oet::Id<Cell> first; };
OET_FIELDS(CensusValues, first)
// jot's value: `self`, the ID of the note it creates.
struct JotValues { oet::Id<Note> self; };
OET_FIELDS(JotValues, self)

oet::Collection<Cell> a{"Misuse.a"};
oet::Collection<Cell> b{"Misuse.b"};

oet::Event<Cell> make0{"make0", oet::create, &Cell::key};
oet::Event<Cell> make1{"make1", oet::create, &Cell::key};
oet::Event<Cell> set_once{"set_once", &Cell::once};
oet::Event<Cell> poke{"poke", &Cell::value};
oet::Event<Cell> poke_again{"poke_again", &Cell::value};
oet::Event<Cell> rejoin{"rejoin"};
oet::Event<Cell> join_b{"join_b"};
oet::Event<Cell> join_b_again{"join_b_again"};
oet::Event<Cell> leave_b{"leave_b"};
oet::Event<Cell> leave_b_again{"leave_b_again"};
oet::Event<Cell> retire{"retire", oet::complete};
oet::Event<Cell> vanish{"vanish", oet::abort};
oet::Event<Cell> poke_late{"poke_late", &Cell::value};
oet::Event<Cell> first_b{"first_b"};
oet::Event<Cell> third{"third"};
oet::Event<Cell> three{"three"};
oet::Event<Cell> small{"small"};
oet::Event<Cell> by_once{"by_once"};
oet::Event<Cell> not_five{"not_five"};
oet::Event<Cell> zero_once{"zero_once"};
oet::Event<oet::none, CensusValues> census{"census"};
oet::Event<Note, JotValues> jot{"jot", oet::create, &Note::v};

namespace {

// One call per Chisel site, in its order, each emitting its site's event; its handler by name.
std::map<std::string, std::pair<oet::Call*, std::function<void()>>> calls;

// The cell with key 0 in a, as Misuse.scala's `zero`; a violation unless exactly one matches.
Cell& zero() { return a.where([](Cell& c) { return c.key == 0u; }).only(); }

// Declares the call `name`, which emits `event` and runs `handler`.
void call(oet::detail::EventBase& event, const char* name, std::function<void()> handler) {
  calls[name] = {new oet::Call(name, {event}), std::move(handler)};
}

// Declares every call, in Misuse.scala's site order, each doing what its Chisel site does.
void declare() {
  call(make0, "make0", [] { Cell& c = make0.create(); c.key = 0; a.add(c); make0.emit(c); });
  call(make1, "make1", [] { Cell& c = make1.create(); c.key = 1; a.add(c); make1.emit(c); });
  call(set_once, "set_once", [] { Cell& z = zero(); z.once = 1; set_once.emit(z); });
  call(poke, "poke", [] { Cell& z = zero(); z.value = 1; poke.emit(z); });
  call(poke_again, "poke_again", [] { Cell& z = zero(); z.value = 2; poke_again.emit(z); });
  call(rejoin, "rejoin", [] { Cell& z = zero(); a.add(z); rejoin.emit(z); });
  call(join_b, "join_b", [] { Cell& z = zero(); b.add(z); join_b.emit(z); });
  call(join_b_again, "join_b_again", [] { Cell& z = zero(); b.add(z); join_b_again.emit(z); });
  call(leave_b, "leave_b", [] { Cell& z = zero(); b.remove(z); leave_b.emit(z); });
  call(leave_b_again, "leave_b_again", [] { Cell& z = zero(); b.remove(z); leave_b_again.emit(z); });
  call(retire, "retire", [] { Cell& z = zero(); a.remove(z); retire.emit(z); });
  call(vanish, "vanish", [] { vanish.emit(zero()); });
  call(poke_late, "poke_late", [] { Cell& z = zero(); z.value = 3; poke_late.emit(z); });
  call(first_b, "first_b", [] { first_b.emit(b.head()); });
  call(third, "third", [] { third.emit(a.at(2)); });
  call(three, "three", [] { three.emit(a.where([](Cell& c) { return c.key == 3u; }).only()); });
  call(small, "small", [] { small.emit(a.where([](Cell& c) { return c.key < 2u; }).only()); });
  call(by_once, "by_once", [] { by_once.emit(a.order_by([](Cell& c) { return c.once.get(); }).head()); });
  call(not_five, "not_five", [] { not_five.emit(a.where([](Cell& c) { return c.once != 5u; }).only()); });
  call(zero_once, "zero_once", [] { zero_once.emit(a.where([](Cell& c) { return c.once == 0u; }).head()); });
  call(census, "census", [] { census.emit({.first = a.head()}); });
  call(jot, "jot", [] { Note& n = jot.create(); n.v = 1; jot.emit(n, {.self = n}); });
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
std::string drain(int fd) {
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

// Runs scenario `s` in a child process, writing its trace as sv_violation<index>.trace in the trace directory, and
// prints and returns whether it ended as expected.
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
  const std::string errors = drain(err[0]);
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
    {"a cell's lifetime", {{"make0", "make1"}, {"poke"}, {"set_once"}, {"join_b"}, {"leave_b"}, {"retire"}}, {},
     "cell.retire subject=cell#0"},
    {"an unset field differs from every value", {{"make0"}, {"not_five"}}, {}, "cell.not_five subject=cell#0"},
    {"an event without a subject naming an object", {{"make1", "make0"}, {"census"}}, {}, " census first=cell#0"},
    {"an untracked object naming itself", {{"jot"}, {"jot"}}, {},
     "note.jot subject=note#1 self=note#1 v=1"},
    {"an unset field equals no value", {{"make0"}, {"zero_once"}},
     {"head found no members of Misuse.a.where(...)"}, nullptr},
    {".only finding no member", {{"make1"}, {"poke"}},
     {"eventtrace: cell.poke at site ", " (call poke, cycle 1): only found 0 members of Misuse.a.where(...)"},
     nullptr},
    {".only finding two members", {{"make0", "make1"}, {"small"}},
     {"only found 2 members of Misuse.a.where(...): cell#0, cell#1"}, nullptr},
    {".head on an empty collection", {{"first_b"}}, {"head found no members of Misuse.b"}, nullptr},
    {".at past the last member", {{"make0"}, {"third"}}, {"at(2) found only 1 members of Misuse.a: cell#0"}, nullptr},
    {"a second setOnce write", {{"make0"}, {"set_once"}, {"set_once"}},
     {"cell#0.once is setOnce and was assigned in an earlier cycle"}, nullptr},
    {"two writes to one field in a cycle", {{"make0"}, {"poke", "poke_again"}},
     {"cell#0.value is assigned twice in one cycle, also by site ", " (cell.poke)"}, nullptr},
    {"adding a member", {{"make0"}, {"rejoin"}}, {"cell#0 is already a member of Misuse.a"}, nullptr},
    {"adding twice in a cycle", {{"make0"}, {"join_b", "join_b_again"}},
     {"cell#0 is added to Misuse.b twice in one cycle"}, nullptr},
    {"removing a non-member", {{"make0"}, {"leave_b"}}, {"cell#0 is not a member of Misuse.b"}, nullptr},
    {"removing twice in a cycle", {{"make0"}, {"join_b"}, {"leave_b", "leave_b_again"}},
     {"cell#0 is removed from Misuse.b twice in one cycle"}, nullptr},
    {"destroying an object another event changes", {{"make0"}, {"poke", "retire"}},
     {"cell#0 is destroyed, but site ", " (cell.poke) also has an effect on it in this cycle"}, nullptr},
    {"changing an object another event destroys", {{"make0"}, {"retire", "poke_late"}},
     {"cell#0 has an effect here, but site ", " (cell.retire) destroys it in this cycle"}, nullptr},
    {"destroying a member", {{"make0"}, {"vanish"}}, {"cell#0 is still in Misuse.a after its destroy"}, nullptr},
    {"ranking by an unset field", {{"make0"}, {"by_once"}}, {"cell#0 has no key to rank by in Misuse.a"}, nullptr},
    {"a call firing twice in a cycle", {{"make0", "make0"}}, {"call make0 fires twice in cycle 0"}, nullptr},
  };

  int failures = 0;
  for (std::size_t i = 0; i < scenarios.size(); ++i) failures += !run(scenarios[i], i);
  if (failures) {
    std::printf("FAIL: %d of %zu scenarios\n", failures, scenarios.size());
    return 1;
  }
  std::printf("PASS: all %zu scenarios: each violation stops the C++ library with its call, cycle, and objects\n",
              scenarios.size());
  return 0;
}
