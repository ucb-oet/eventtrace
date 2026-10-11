#include "eventtrace/runtime.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace eventtrace {

Runtime::Runtime(Handler handler, const unsigned* site_words, unsigned site_count, unsigned object_types)
    : handler_(handler), site_words_(site_words), site_count_(site_count), next_object_ids_(object_types, 0) {}

void Runtime::accept(long long cycle, unsigned site, const uint32_t* payload, unsigned words) {
  if (finished_) fail("record from site %u at cycle %lld arrived after the stream ended", site, cycle);
  if (site >= site_count_) fail("record from unknown site %u at cycle %lld", site, cycle);
  if (words != site_words_[site])
    fail("site %u sent %u payload words, but the generated site table expects %u; is the build stale?", site, words,
         site_words_[site]);
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
  std::fflush(stdout);
}

void Runtime::flush() {
  std::sort(records_.begin(), records_.end(), [](const Record& a, const Record& b) { return a.site < b.site; });
  for (std::size_t i = 0; i < records_.size(); ++i) {
    const Record& r = records_[i];
    // A site fires at most once per cycle; two sinks sharing a site ID, as with Definition/Instance, break that.
    if (i > 0 && records_[i - 1].site == r.site) fail("site %u fired twice in cycle %lld", r.site, cycle_);
    if (!handler_(*this, cycle_, next_event_id_++, r.site, words_.data() + r.offset))
      fail("the generated handler does not know site %u", r.site);
  }
  records_.clear();
  words_.clear();
}

void Runtime::fail(const char* format, ...) const {
  std::fflush(stdout);
  std::fputs("eventtrace: ", stderr);
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
  std::fputc('\n', stderr);
  std::abort();
}

}  // namespace eventtrace
