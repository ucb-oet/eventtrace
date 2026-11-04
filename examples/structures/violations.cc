// Drives the structures Misuse fixture's generated handlers with synthetic records, so no simulator can hide a
// bookkeeping error in a queue, a ROB, or a map. Each scenario runs in a child process, which must stop with its
// expected violation or, for the passing scenarios, finish normally with its expected records.
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "eventtrace_runtime.h"
#include "eventtrace_sites.h"  // Generated from Misuse.scala.

namespace {

// The site of the event cell.<name>, or of <name> if it is qualified or has no subject.
unsigned site(const std::string& name) {
  for (unsigned s = 0; s < eventtrace::sites::kSiteCount; ++s)
    if ("cell." + name == eventtrace::sites::kSiteEvents[s].name || name == eventtrace::sites::kSiteEvents[s].name)
      return s;
  std::fprintf(stderr, "no site emits %s\n", name.c_str());
  std::exit(2);
}

// A sequence of synthetic records and how its run must end: stopped with a violation containing `violation`, or, if
// that is null, finished normally with a trace containing `records`.
struct Scenario {
  const char* label;
  std::vector<std::vector<std::string>> cycles;  // the events firing in cycles 0, 1, ...
  const char* violation;                         // part of the expected violation, or null for a passing run
  const char* records;                           // for a passing run, part of its expected records
};

// From the command line: the command that prints a trace as text given its manifest and path, the fixture's
// manifest, and a directory for the scenarios' traces.
std::string dump, manifest, trace_dir;

// Everything readable from `fd`, which it then closes.
std::string drain(int fd) {
  std::string s;
  char buffer[4096];
  for (ssize_t n; (n = read(fd, buffer, sizeof buffer)) > 0;) s.append(buffer, static_cast<std::size_t>(n));
  close(fd);
  return s;
}

// The trace at `path` as text.
std::string text_of(const std::string& path) {
  const std::string command = dump + " " + manifest + " " + path;
  std::FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) std::abort();
  std::string text;
  char buffer[4096];
  for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0;) text.append(buffer, n);
  pclose(pipe);
  return text;
}

// Runs scenario `s` in a child process, writing its trace as violation<index>.trace in the trace directory, and
// prints and returns whether it ended as expected.
bool run(const Scenario& s, std::size_t index) {
  const std::string trace = trace_dir + "/violation" + std::to_string(index) + ".trace";
  int err[2];
  if (pipe(err) != 0) std::abort();
  std::fflush(stdout);  // a child would otherwise print what is still buffered again
  const pid_t pid = fork();
  if (pid == 0) {
    dup2(err[1], 2);
    eventtrace::sites::Tracker tracker;
    eventtrace::Runtime runtime(tracker);
    runtime.open(trace.c_str());
    for (std::size_t cycle = 0; cycle < s.cycles.size(); ++cycle)
      for (const std::string& name : s.cycles[cycle]) runtime.accept(static_cast<long long>(cycle), site(name), nullptr, 0);
    runtime.finish();
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
  if (s.violation) ok = aborted && errors.find(s.violation) != std::string::npos;
  else ok = finished && errors.empty() && records.find(s.records) != std::string::npos;
  const std::string shown = s.violation ? errors.substr(0, errors.find('\n')) : std::string("finished normally");
  std::printf("%s: %s: %s\n", ok ? "PASS" : "FAIL", s.label, ok ? shown.c_str() : (errors + records).c_str());
  return ok;
}

}  // namespace

// Takes the dump command, the fixture's manifest, and the trace directory; checks that no site has a payload, then
// runs every scenario and fails if any did not end as expected.
int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <dump command> <manifest> <trace directory>\n", argv[0]);
    return 2;
  }
  dump = argv[1];
  manifest = argv[2];
  trace_dir = argv[3];
  for (unsigned s = 0; s < eventtrace::sites::kSiteCount; ++s)
    if (eventtrace::sites::kPayloadWords[s] != 0) {
      std::printf("FAIL: site %u has a payload, which these synthetic records do not supply\n", s);
      return 1;
    }

  const std::vector<Scenario> scenarios = {
    {"a queue's lifetime", {{"enq0"}, {"enq1"}, {"deq"}, {"deq"}}, nullptr, "cell.deq subject=cell#1"},
    {"a queue cleared", {{"enq0"}, {"enq1"}, {"flush"}, {"enq0"}}, nullptr, "cell.flush subject=cell#1"},
    {"two pops in one cycle", {{"enq0"}, {"enq1"}, {"deq", "deq1"}}, nullptr, "cell.deq1 subject=cell#1"},
    {"the youngest member dropped", {{"enq0"}, {"enq1"}, {"trim1"}, {"deq"}}, nullptr, "cell.deq subject=cell#0"},
    {"a pop and a drop in one cycle", {{"enq0"}, {"enq1"}, {"deq", "trim1"}}, nullptr, "cell.trim1 subject=cell#1"},
    {"a ROB's keys wrapping", {{"alloc0"}, {"alloc1"}, {"commit"}, {"alloc2"}, {"commit"}, {"alloc3"}, {"commit"},
     {"alloc0"}}, nullptr, "cell.alloc0 subject=cell#4 key=0"},
    {"a rollback moving the tail back", {{"alloc0"}, {"alloc1"}, {"squash1"}, {"alloc1"}}, nullptr,
     "cell.alloc1 subject=cell#2 key=1"},
    {"a rollback to the tail itself", {{"alloc0"}, {"squash1"}, {"alloc1"}}, nullptr, "cell.alloc1 subject=cell#1 key=1"},
    {"a map key freed and taken in one cycle", {{"put_a"}, {"take", "put_b"}}, nullptr, "cell.put_b subject=cell#1 key=1"},
    {"a pop past the cycle's other pops", {{"enq0"}, {"enq1"}, {"deq1"}},
     "cell#1 is popped from Misuse.q at position 1, but this cycle's 1 pop must take its first member", nullptr},
    {"a queue past its entries", {{"enq0"}, {"enq1"}, {"enq2"}},
     "Misuse.q holds 3 members at the end of the cycle, more than its 2 entries", nullptr},
    {"a pop and a clear taking one member", {{"enq0"}, {"deq_keep", "drain"}},
     "cell#0 is removed from Misuse.q twice in one cycle", nullptr},
    {"dropping more members than there are", {{"enq0"}, {"enq1"}, {"trim3"}},
     "dropYoungest(3) found only 2 members of Misuse.q", nullptr},
    {"a ROB key out of sequence", {{"alloc0"}, {"alloc2"}},
     "cell#1 joins Misuse.r with key 2, but its tail allocates 1 next", nullptr},
    {"a ROB key without a member", {{"alloc0"}, {"find1"}}, "Misuse.r has no member with key 1", nullptr},
    {"a rollback to a key neither live nor the tail", {{"alloc0"}, {"squash3"}},
     "rolls Misuse.r back to key 3, which is neither a live key nor its tail, 1", nullptr},
    {"a ROB past its entries", {{"alloc0"}, {"alloc1"}, {"alloc2"}},
     "Misuse.r holds 3 members at the end of the cycle, more than its 2 entries", nullptr},
    {"a map key already held", {{"put_a"}, {"put_b"}}, "cell#1 joins Misuse.m with key 1, which cell#0 holds", nullptr},
    {"a map key without a member", {{"find0"}}, "Misuse.m has no member with key 0", nullptr},
  };

  int failures = 0;
  for (std::size_t i = 0; i < scenarios.size(); ++i) failures += !run(scenarios[i], i);
  if (failures) {
    std::printf("FAIL: %d of %zu scenarios\n", failures, scenarios.size());
    return 1;
  }
  std::printf("PASS: all %zu scenarios: each violation of a queue, ROB, or map stops the runtime with its site, "
              "cycle, and objects\n", scenarios.size());
  return 0;
}
