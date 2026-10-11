#include "eventtrace_runtime.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace eventtrace {

Runtime::Runtime(Sites& sites) : sites_(sites), next_object_ids_(sites.object_type_count(), 0) {}

void Runtime::open(const char* path) {
  if (out_) return;
  out_ = std::fopen(path, "w");
  if (!out_) fail("cannot open %s for the trace records", path);
}

void Runtime::accept(long long cycle, unsigned site, const uint32_t* payload, unsigned words) {
  if (!out_) return;
  if (finished_) fail("record from site %u at cycle %lld arrived after the stream ended", site, cycle);
  if (site >= sites_.site_count()) fail("record from unknown site %u at cycle %lld", site, cycle);
  if (words != sites_.payload_words(site))
    fail("site %u sent %u payload words, but the generated site table expects %u; is the build stale?", site, words,
         sites_.payload_words(site));
  if (cycle < cycle_) fail("record from site %u at cycle %lld arrived after cycle %lld began", site, cycle, cycle_);
  if (cycle > cycle_) {
    flush();
    cycle_ = cycle;
  }
  records_.push_back({site, words_.size()});
  words_.insert(words_.end(), payload, payload + words);
}

void Runtime::finish() {
  if (finished_) return;
  flush();
  finished_ = true;
  if (out_) std::fflush(out_);
}

void Runtime::flush() {
  if (cycle_ < 0) return;
  std::sort(records_.begin(), records_.end(), [](const Record& a, const Record& b) { return a.site < b.site; });
  for (std::size_t i = 0; i < records_.size(); ++i) {
    const Record& r = records_[i];
    // A site fires at most once per cycle; two sinks sharing a site ID, as with Definition/Instance, break that.
    if (i > 0 && records_[i - 1].site == r.site) fail("site %u fired twice in cycle %lld", r.site, cycle_);
    sites_.handle(*this, r.site, words_.data() + r.offset);
  }
  sites_.commit(*this);
  records_.clear();
  words_.clear();
}

void Runtime::violation(unsigned site, const char* format, ...) const {
  if (out_) std::fflush(out_);
  std::fprintf(stderr, "eventtrace: %s at site %u (cycle %lld): ", sites_.site_name(site), site, cycle_);
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
  std::fputc('\n', stderr);
  std::abort();
}

void Runtime::fail(const char* format, ...) const {
  if (out_) std::fflush(out_);
  std::fputs("eventtrace: ", stderr);
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
  std::fputc('\n', stderr);
  std::abort();
}

}  // namespace eventtrace
