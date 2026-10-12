// Feeds the runtime synthetic records, so its ordering does not depend on the order a simulator happens to use, and
// checks the trace it writes byte by byte against protobuf's wire format.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
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

// Site 0 has no payload; sites 1 to 3 have one word, and site 3 has two.
class FakeSites final : public eventtrace::Sites {
 public:
  unsigned site_count() const override { return 4; }
  unsigned payload_words(unsigned site) const override { return kWords[site]; }
  const char* site_name(unsigned) const override { return "fake"; }
  unsigned object_type_count() const override { return 2; }
  const char* manifest_sha256() const override { return "abc"; }

  void handle(eventtrace::Runtime& rt, unsigned site, const uint32_t* words) override {
    unsigned long long object_id = site == 2 ? rt.create(0) : site == 3 ? rt.create(1) : ~0ull;
    handled.push_back({rt.cycle(), rt.next_event_id(), site, site == 0 ? 0u : words[0], object_id});
    // Site 3's events carry a subject, a value, a negative value, and an absent field 2.
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
};

int failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
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
  if (failures) return 1;
  std::printf("PASS: records are ignored until a file is open, records are handled in (cycle, site) order with "
              "event IDs 0 to 4, each object type numbers its objects from 0, payloads are copied, each cycle commits "
              "once, open() and finish() are idempotent, and the trace is byte-exact protobuf\n");
  return 0;
}
