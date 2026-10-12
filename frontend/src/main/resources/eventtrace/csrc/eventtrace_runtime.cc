#include "eventtrace_runtime.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace eventtrace {

// Protobuf wire types and the field numbers of eventtrace.proto.
namespace {
constexpr unsigned kVarint = 0, kLength = 2;
constexpr unsigned kRecordHeader = 1, kRecordCycle = 2;
constexpr unsigned kHeaderVersion = 1, kHeaderManifest = 2;
constexpr unsigned kCycleCycle = 1, kCycleEvents = 2;
constexpr unsigned kEventSite = 1, kEventSubject = 2, kEventValues = 3, kEventAbsent = 4;
constexpr unsigned kFormatVersion = 1;

// Every field number above is below 16, so each tag is one byte.
void put_tag(std::string& s, unsigned field, unsigned wire_type) {
  s.push_back(static_cast<char>(field << 3 | wire_type));
}
}  // namespace

void TraceWriter::put_varint(std::string& s, uint64_t v) {
  for (; v >= 0x80; v >>= 7) s.push_back(static_cast<char>(v | 0x80));
  s.push_back(static_cast<char>(v));
}

void TraceWriter::put_message(std::string& s, unsigned field, const std::string& message) {
  put_tag(s, field, kLength);
  put_varint(s, message.size());
  s += message;
}

void TraceWriter::begin(unsigned site) {
  site_ = site;
  subject_ = 0;
  absent_ = 0;
  values_.clear();
}

// Proto3 leaves out fields at their defaults, so a zero site, subject, or mask, or no values, takes no bytes.
void TraceWriter::end() {
  std::string event;
  if (site_) { put_tag(event, kEventSite, kVarint); put_varint(event, site_); }
  if (subject_) { put_tag(event, kEventSubject, kVarint); put_varint(event, subject_); }
  if (!values_.empty()) put_message(event, kEventValues, values_);
  if (absent_) { put_tag(event, kEventAbsent, kVarint); put_varint(event, absent_); }
  put_message(events_, kCycleEvents, event);
}

std::string TraceWriter::header(const char* manifest_sha256) {
  std::string header;
  put_tag(header, kHeaderVersion, kVarint);
  put_varint(header, kFormatVersion);
  put_message(header, kHeaderManifest, manifest_sha256);
  std::string record, framed;
  put_message(record, kRecordHeader, header);
  put_varint(framed, record.size());
  return framed + record;
}

std::string TraceWriter::take_cycle(long long cycle) {
  if (events_.empty()) return {};
  std::string message;
  if (cycle) { put_tag(message, kCycleCycle, kVarint); put_varint(message, static_cast<uint64_t>(cycle)); }
  message += events_;
  events_.clear();
  std::string record, framed;
  put_message(record, kRecordCycle, message);
  put_varint(framed, record.size());
  return framed + record;
}

Runtime::Runtime(Sites& sites) : sites_(sites), next_object_ids_(sites.object_type_count(), 0) {}

void Runtime::open(const char* path) {
  if (out_) return;
  out_ = std::fopen(path, "wb");
  if (!out_) fail("cannot open %s for the trace", path);
  const std::string header = TraceWriter::header(sites_.manifest_sha256());
  std::fwrite(header.data(), 1, header.size(), out_);
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
  const std::string record = writer_.take_cycle(cycle_);
  std::fwrite(record.data(), 1, record.size(), out_);
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
