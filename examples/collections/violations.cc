// Drives the Misuse fixture's generated handlers with synthetic records, so no simulator can hide a bookkeeping
// error. Each scenario runs in a child process, which must stop with its expected violation or, for the passing
// scenarios, finish normally with its expected records.
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "eventtrace/runtime.h"
#include "eventtrace_sites.h"  // Generated from Misuse.scala.

namespace {

// The site of the event cell.<name>, or of <name> if it is qualified or has no subject.
unsigned site(const std::string& name) {
  for (unsigned s = 0; s < eventtrace::sites::kSiteCount; ++s)
    if ("cell." + name == eventtrace::sites::kSiteNames[s] || name == eventtrace::sites::kSiteNames[s]) return s;
  std::fprintf(stderr, "no site emits %s\n", name.c_str());
  std::exit(2);
}

struct Scenario {
  const char* label;
  std::vector<std::vector<std::string>> cycles;  // the events firing in cycles 0, 1, ...
  const char* violation;                         // part of the expected violation, or null for a passing run
  const char* records;                           // for a passing run, part of its expected records
};

std::string drain(int fd) {
  std::string s;
  char buffer[4096];
  for (ssize_t n; (n = read(fd, buffer, sizeof buffer)) > 0;) s.append(buffer, static_cast<std::size_t>(n));
  close(fd);
  return s;
}

bool run(const Scenario& s) {
  int out[2], err[2];
  if (pipe(out) != 0 || pipe(err) != 0) std::abort();
  const pid_t pid = fork();
  if (pid == 0) {
    dup2(out[1], 1);
    dup2(err[1], 2);
    eventtrace::sites::Tracker tracker;
    eventtrace::Runtime runtime(tracker);
    for (std::size_t cycle = 0; cycle < s.cycles.size(); ++cycle)
      for (const std::string& name : s.cycles[cycle]) runtime.accept(static_cast<long long>(cycle), site(name), nullptr, 0);
    runtime.finish();
    std::exit(0);
  }
  close(out[1]);
  close(err[1]);
  // The children write little, so reading one pipe after the other cannot block them.
  const std::string records = drain(out[0]);
  const std::string errors = drain(err[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  const bool aborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
  const bool finished = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  bool ok;
  if (s.violation) ok = aborted && errors.find(s.violation) != std::string::npos;
  else ok = finished && errors.empty() && records.find(s.records) != std::string::npos;
  const std::string shown = s.violation ? errors.substr(0, errors.find('\n')) : std::string("finished normally");
  std::printf("%s: %s: %s\n", ok ? "PASS" : "FAIL", s.label, ok ? shown.c_str() : (errors + records).c_str());
  return ok;
}

}  // namespace

int main() {
  for (unsigned s = 0; s < eventtrace::sites::kSiteCount; ++s)
    if (eventtrace::sites::kPayloadWords[s] != 0) {
      std::printf("FAIL: site %u has a payload, which these synthetic records do not supply\n", s);
      return 1;
    }

  const std::vector<Scenario> scenarios = {
    {"a cell's lifetime", {{"make0", "make1"}, {"poke"}, {"set_once"}, {"join_b"}, {"leave_b"}, {"retire"}}, nullptr,
     "cell.retire subject=cell#0"},
    {"an unset field differs from every value", {{"make0"}, {"not_five"}}, nullptr, "cell.not_five subject=cell#0"},
    {"an event without a subject naming an object", {{"make1", "make0"}, {"census"}}, nullptr,
     "site_id=20 census first=cell#0"},
    {"an untracked object naming itself", {{"note.jot"}, {"note.jot"}}, nullptr,
     "note.jot subject=note#1 self=note#1 v=1"},
    {"an unset field equals no value", {{"make0"}, {"zero_once"}},
     "head found no members of Misuse.a matching once == 0", nullptr},
    {".only finding no member", {{"make1"}, {"poke"}},
     "eventtrace: cell.poke at site 3 (cycle 1): only found 0 members of Misuse.a matching key == 0", nullptr},
    {".only finding two members", {{"make0", "make1"}, {"small"}},
     "only found 2 members of Misuse.a matching key < 2: cell#0, cell#1", nullptr},
    {".head on an empty collection", {{"first_b"}}, "head found no members of Misuse.b", nullptr},
    {".at past the last member", {{"make0"}, {"third"}}, "at(2) found only 1 members of Misuse.a: cell#0", nullptr},
    {"a second setOnce write", {{"make0"}, {"set_once"}, {"set_once"}},
     "cell#0.once is setOnce and was assigned in an earlier cycle", nullptr},
    {"two writes to one field in a cycle", {{"make0"}, {"poke", "poke_again"}},
     "cell#0.value is assigned twice in one cycle, also by site 3 (cell.poke)", nullptr},
    {"adding a member", {{"make0"}, {"rejoin"}}, "cell#0 is already a member of Misuse.a", nullptr},
    {"adding twice in a cycle", {{"make0"}, {"join_b", "join_b_again"}},
     "cell#0 is added to Misuse.b twice in one cycle", nullptr},
    {"removing a non-member", {{"make0"}, {"leave_b"}}, "cell#0 is not a member of Misuse.b", nullptr},
    {"removing twice in a cycle", {{"make0"}, {"join_b"}, {"leave_b", "leave_b_again"}},
     "cell#0 is removed from Misuse.b twice in one cycle", nullptr},
    {"destroying an object another event changes", {{"make0"}, {"poke", "retire"}},
     "cell#0 is destroyed, but site 3 (cell.poke) also has an effect on it in this cycle", nullptr},
    {"changing an object another event destroys", {{"make0"}, {"retire", "poke_late"}},
     "cell#0 has an effect here, but site 10 (cell.retire) destroys it in this cycle", nullptr},
    {"destroying a member", {{"make0"}, {"vanish"}}, "cell#0 is still in Misuse.a after its destroy", nullptr},
    {"ranking by an unset field", {{"make0"}, {"by_once"}}, "cell#0 has no once to rank by in Misuse.a", nullptr},
    {"a site firing twice in a cycle", {{"make0", "make0"}}, "site 0 fired twice in cycle 0", nullptr},
  };

  int failures = 0;
  for (const Scenario& s : scenarios) failures += !run(s);
  if (failures) {
    std::printf("FAIL: %d of %zu scenarios\n", failures, scenarios.size());
    return 1;
  }
  std::printf("PASS: all %zu scenarios: each violation stops the runtime with its site, cycle, and objects\n",
              scenarios.size());
  return 0;
}
