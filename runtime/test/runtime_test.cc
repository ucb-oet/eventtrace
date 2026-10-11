// Feeds the runtime synthetic records, so its ordering does not depend on the order a simulator happens to use.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "eventtrace/runtime.h"

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

  void handle(eventtrace::Runtime& rt, unsigned site, const uint32_t* words) override {
    unsigned long long object_id = site == 2 ? rt.create(0) : site == 3 ? rt.create(1) : ~0ull;
    handled.push_back({rt.cycle(), rt.next_event_id(), site, site == 0 ? 0u : words[0], object_id});
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
  if (failures) return 1;
  std::printf("PASS: records are handled in (cycle, site) order with event IDs 0 to 4, each object type numbers its "
              "objects from 0, payloads are copied, each cycle commits once, and finish() is idempotent\n");
  return 0;
}
