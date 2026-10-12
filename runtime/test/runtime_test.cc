// Feeds the runtime synthetic records, so its ordering does not depend on the order a simulator happens to use, and
// checks the trace it writes byte by byte against protobuf's wire format, then the recording window's triggers.
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "eventtrace_runtime.h"

namespace {

struct Handled {
  long long cycle;
  unsigned long long event_id;
  unsigned site;
  uint32_t first_word;
  unsigned long long object_id;  // for sites 2 and 3, which create objects of types 0 and 1
};

// Site 0 has no payload; sites 1 to 3 have one word, and site 3 has two. Only site 3's event, sample, has fields.
class FakeSites final : public eventtrace::Sites {
 public:
  unsigned site_count() const override { return 4; }
  unsigned payload_words(unsigned site) const override { return kWords[site]; }
  const eventtrace::EventInfo& event(unsigned site) const override { return kEvents[site]; }
  unsigned object_type_count() const override { return 2; }
  const char* manifest_sha256() const override { return "abc"; }

  void handle(eventtrace::Runtime& rt, unsigned site, const uint32_t* words) override {
    unsigned long long object_id = site == 2 ? rt.create(0) : site == 3 ? rt.create(1) : ~0ull;
    handled.push_back({rt.cycle(), rt.next_event_id(), site, site == 0 ? 0u : words[0], object_id});
    // Site 3's events carry a subject, x from the payload, y of -2, and an absent z.
    eventtrace::TraceWriter& w = rt.writer();
    w.begin(site);
    if (site == 3) {
      w.subject(object_id);
      w.value(words[0]);
      w.value_signed(-2);
      w.absent(2);
    }
    w.end();
  }

  void commit(eventtrace::Runtime& rt) override { commits.push_back(rt.cycle()); }

  std::vector<Handled> handled;
  std::vector<long long> commits;

 private:
  static constexpr unsigned kWords[] = {0, 1, 1, 2};
  static constexpr eventtrace::EnumVariant kLevels[] = {{"Low", 0}, {"High", 1}};
  static constexpr eventtrace::FieldInfo kSampleFields[] = {
      {"x", false, nullptr, 0}, {"y", true, nullptr, 0}, {"z", false, kLevels, 2}};
  static constexpr eventtrace::EventInfo kEvents[] = {
      {"tick", nullptr, 0}, {"pulse", nullptr, 0}, {"spawn", nullptr, 0}, {"sample", kSampleFields, 3}};
};

int failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

// The cycles of a trace's records after its header. A cycle Record is tag 12 and the Cycle's length, then the Cycle's
// stamp, tag 08, which proto3 leaves out for cycle 0.
std::vector<long long> cycles_in(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  const std::string data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  std::size_t pos = 0;
  const auto varint = [&] {
    uint64_t v = 0;
    for (unsigned shift = 0;; shift += 7) {
      const unsigned char b = static_cast<unsigned char>(data[pos++]);
      v |= uint64_t(b & 0x7f) << shift;
      if (b < 0x80) return v;
    }
  };
  std::vector<long long> cycles;
  while (pos < data.size()) {
    const std::size_t size = varint();
    const std::size_t end = pos + size;
    if (data[pos] == 0x12) {
      ++pos;
      varint();
      cycles.push_back(data[pos] == 0x08 ? (++pos, static_cast<long long>(varint())) : 0);
    }
    pos = end;
  }
  return cycles;
}

// Records a sample with x = 10 * cycle in cycles 5, 6, 8, and 9 through a window, and returns the cycles written.
std::vector<long long> window(const char* start, const char* stop) {
  static int run = 0;
  const std::string path = "build/runtime_test_window" + std::to_string(run++) + ".trace";
  FakeSites sites;
  eventtrace::Runtime runtime(sites);
  runtime.open(path.c_str(), start, stop);
  for (const long long cycle : {5, 6, 8, 9}) {
    const uint32_t payload[2] = {static_cast<uint32_t>(10 * cycle), 0};
    runtime.accept(cycle, 3, payload, 2);
  }
  runtime.finish();
  return cycles_in(path);
}

// Opens a runtime with a start trigger in a child process, which must stop with an error containing `error`.
void expect_rejected(const char* start, const char* error) {
  int err[2];
  if (pipe(err) != 0) std::abort();
  const pid_t child = fork();
  if (child == 0) {
    dup2(err[1], 2);
    FakeSites sites;
    eventtrace::Runtime runtime(sites);
    runtime.open("build/runtime_test_rejected.trace", start, "");
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
    std::printf("FAIL: +eventtrace_start=%s should stop with \"%s\", but printed \"%s\"\n", start, error,
                message.c_str());
    ++failures;
  }
}

}  // namespace

int main() {
  FakeSites sites;
  eventtrace::Runtime runtime(sites);

  // Without +eventtrace=<path>, no file is open, and records are ignored. Only the first open counts, so the second,
  // whose directory does not exist, does nothing.
  runtime.accept(3, 0, nullptr, 0);
  runtime.accept(4, 0, nullptr, 0);
  expect(sites.handled.empty() && sites.commits.empty(), "records are ignored until a file is open");
  runtime.open("build/runtime_test.trace");
  runtime.open("/nonexistent/eventtrace.log");

  // Cycle 5 arrives as sites 3, 0, 2; cycle 6 as 1, 3. The payload buffer is reused, as a simulator's would be.
  uint32_t buffer[2];
  buffer[0] = 30; buffer[1] = 31; runtime.accept(5, 3, buffer, 2);
  runtime.accept(5, 0, nullptr, 0);
  buffer[0] = 20; runtime.accept(5, 2, buffer, 1);
  expect(sites.handled.empty(), "nothing is handled before its cycle is complete");
  buffer[0] = 11; runtime.accept(6, 1, buffer, 1);
  buffer[0] = 36; buffer[1] = 37; runtime.accept(6, 3, buffer, 2);
  runtime.finish();
  runtime.finish();

  const unsigned long long none = ~0ull;
  const Handled want[] = {{5, 0, 0, 0, none}, {5, 1, 2, 20, 0}, {5, 2, 3, 30, 0}, {6, 3, 1, 11, none}, {6, 4, 3, 36, 1}};
  expect(sites.handled.size() == 5, "five records are handled, once each, although finish() was called twice");
  for (std::size_t i = 0; i < sites.handled.size() && i < 5; ++i) {
    const Handled& h = sites.handled[i];
    if (h.cycle != want[i].cycle || h.event_id != want[i].event_id || h.site != want[i].site ||
        h.first_word != want[i].first_word || h.object_id != want[i].object_id) {
      std::printf("FAIL: record %zu is cycle %lld event %llu site %u word %u object %llu\n", i, h.cycle, h.event_id,
                  h.site, h.first_word, h.object_id);
      ++failures;
    }
  }
  expect(sites.commits == std::vector<long long>({5, 6}), "each cycle is committed once, after its records");

  // Each record is framed by its length. Field tags: Record header 0a and cycle 12; Header version 08 and hash 12;
  // Cycle stamp 08 and events 12; Event site 08, subject 10, values 1a, and absent 20. Proto3 leaves out zeros, so
  // site 0's event is empty and object 0 is no subject. -2 is ZigZag 3; absent field 2 is the mask 4.
  const std::vector<unsigned char> want_trace = {
      0x09, 0x0a, 0x07, 0x08, 0x01, 0x12, 0x03, 'a', 'b', 'c',                    // header
      0x14, 0x12, 0x12, 0x08, 0x05,                                               // cycle 5
      0x12, 0x00,                                                                 //   site 0
      0x12, 0x02, 0x08, 0x02,                                                     //   site 2
      0x12, 0x08, 0x08, 0x03, 0x1a, 0x02, 0x1e, 0x03, 0x20, 0x04,                 //   site 3, object 0
      0x14, 0x12, 0x12, 0x08, 0x06,                                               // cycle 6
      0x12, 0x02, 0x08, 0x01,                                                     //   site 1
      0x12, 0x0a, 0x08, 0x03, 0x10, 0x01, 0x1a, 0x02, 0x24, 0x03, 0x20, 0x04,     //   site 3, object 1
  };
  std::ifstream file("build/runtime_test.trace", std::ios::binary);
  const std::vector<unsigned char> trace{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  expect(trace == want_trace, "the trace is a header, then one record per cycle, in protobuf's wire format");

  // Each trigger's cycle is recorded, and the stop trigger counts only events from the cycle the window opens.
  using Cycles = std::vector<long long>;
  expect(window("", "") == Cycles({5, 6, 8, 9}), "without triggers, every cycle is written");
  expect(window("cycle:7", "") == Cycles({8, 9}), "a start cycle without records opens at the next that has some");
  expect(window("cycle:6", "cycle:7") == Cycles({6}), "a stop cycle without records closes before the next");
  expect(window("event:sample,x=60", "event:sample,x=0x50") == Cycles({6, 8}), "event triggers match fields");
  expect(window("event:sample,x=60", "event:sample,y=-2") == Cycles({6}),
         "a stop matching earlier is ignored, and one matching in the start cycle closes after it");
  expect(window("event:sample", "cycle:8") == Cycles({5, 6, 8}), "an event trigger without fields fires first");
  expect(window("event:sample,z=High", "").empty(), "an absent field matches no value, so the window never opens");
  expect(window("event:sample,z=0", "").empty(), "an enum field takes a number too, but an absent one never matches");
  expect_rejected("frame:3", "a trigger is cycle:<n> or event:");
  expect_rejected("cycle:x", "the cycle is not a number");
  expect_rejected("event:nothing", "no event is named nothing");
  expect_rejected("event:sample,w=1", "event sample has no field w");
  expect_rejected("event:sample,x", "x is not <field>=<value>");
  expect_rejected("event:sample,z=Middle", "Middle is not a variant of field z");
  expect_rejected("event:sample,x=-1", "-1 is not a value of field x");
  if (failures) return 1;
  std::printf("PASS: records are ignored until a file is open, records are handled in (cycle, site) order with "
              "event IDs 0 to 4, each object type numbers its objects from 0, payloads are copied, each cycle commits "
              "once, open() and finish() are idempotent, the trace is byte-exact protobuf, recording windows open and close on "
              "cycles and on events' fields, and bad triggers stop the run\n");
  return 0;
}
