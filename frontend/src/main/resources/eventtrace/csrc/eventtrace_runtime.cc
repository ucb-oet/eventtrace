#include "eventtrace_runtime.h"

#include <algorithm>
#include <bitset>
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

// The trace holds only the present fields' values, so a field's value follows those of the present fields before it.
bool Trigger::matches(unsigned site, const std::string& values, uint64_t absent) const {
  if (site >= sites.size() || !sites[site]) return false;
  std::vector<uint64_t> present;
  uint64_t v = 0;
  unsigned shift = 0;
  for (const char c : values) {
    v |= uint64_t(static_cast<unsigned char>(c) & 0x7f) << shift;
    shift += 7;
    if (!(c & 0x80)) {
      present.push_back(v);
      v = shift = 0;
    }
  }
  for (const auto& [field, value] : fields) {
    if (field < 64 && (absent >> field & 1)) return false;
    const uint64_t before = field < 64 ? absent & ((uint64_t{1} << field) - 1) : absent;
    const std::size_t index = field - std::bitset<64>(before).count();
    if (index >= present.size() || present[index] != value) return false;
  }
  return true;
}

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
  for (Trigger* t : watching_)
    if (!t->hit && t->matches(site_, values_, absent_)) t->hit = true;
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

void Runtime::open(const char* path, const char* start, const char* stop) {
  if (out_) return;
  start_ = parse_trigger("eventtrace_start", start);
  stop_ = parse_trigger("eventtrace_stop", stop);
  window_ = start_.mode == Trigger::Mode::None ? Window::Open : Window::Waiting;
  watch();
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
  if (!out_) return;
  std::fflush(out_);
  if (window_ == Window::Waiting)
    std::fprintf(stderr, "eventtrace: +eventtrace_start=%s never fired, so the trace has no events\n", start_.spec.c_str());
  else if (window_ == Window::Open && stop_.mode != Trigger::Mode::None)
    std::fprintf(stderr, "eventtrace: +eventtrace_stop=%s never fired, so the trace runs to the end\n",
                 stop_.spec.c_str());
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
  const auto fires = [this](const Trigger& t) {
    return t.mode == Trigger::Mode::Cycle ? cycle_ >= t.cycle : t.hit;
  };
  if (window_ == Window::Waiting && fires(start_)) window_ = Window::Open;
  // A stop cycle without records closes the window before the next cycle that has some.
  if (window_ == Window::Open && stop_.mode == Trigger::Mode::Cycle && cycle_ > stop_.cycle) window_ = Window::Closed;
  if (window_ == Window::Open) {
    std::fwrite(record.data(), 1, record.size(), out_);
    if (stop_.mode != Trigger::Mode::None && fires(stop_)) {
      window_ = Window::Closed;
      std::fflush(out_);
    }
  }
  start_.hit = stop_.hit = false;
  watch();
  records_.clear();
  words_.clear();
}

// Matches events against only the event triggers that can still fire.
void Runtime::watch() {
  writer_.watching_.clear();
  if (window_ == Window::Waiting && start_.mode == Trigger::Mode::Event) writer_.watching_.push_back(&start_);
  if (window_ != Window::Closed && stop_.mode == Trigger::Mode::Event) writer_.watching_.push_back(&stop_);
}

Trigger Runtime::parse_trigger(const char* plusarg, const char* spec) const {
  Trigger t;
  t.spec = spec;
  if (t.spec.empty()) return t;
  const auto number = [&](const std::string& text, bool is_signed, uint64_t& out) {
    if (text.empty() || (!is_signed && text[0] == '-')) return false;
    char* end = nullptr;
    out = is_signed ? static_cast<uint64_t>(std::strtoll(text.c_str(), &end, 0)) : std::strtoull(text.c_str(), &end, 0);
    return *end == '\0';
  };
  if (t.spec.rfind("cycle:", 0) == 0) {
    uint64_t cycle;
    if (!number(t.spec.substr(6), false, cycle)) fail("+%s=%s: the cycle is not a number", plusarg, spec);
    t.mode = Trigger::Mode::Cycle;
    t.cycle = static_cast<long long>(cycle);
    return t;
  }
  if (t.spec.rfind("event:", 0) != 0)
    fail("+%s=%s: a trigger is cycle:<n> or event:<name>[,<field>=<value>...]", plusarg, spec);
  t.mode = Trigger::Mode::Event;
  std::vector<std::string> parts;
  for (std::size_t from = 6, comma; from <= t.spec.size(); from = comma + 1) {
    comma = std::min(t.spec.find(',', from), t.spec.size());
    parts.push_back(t.spec.substr(from, comma - from));
  }
  const EventInfo* event = nullptr;
  t.sites.assign(sites_.site_count(), false);
  for (unsigned s = 0; s < sites_.site_count(); ++s) {
    if (parts[0] != sites_.event(s).name) continue;
    t.sites[s] = true;
    event = &sites_.event(s);
  }
  if (!event) fail("+%s=%s: no event is named %s", plusarg, spec, parts[0].c_str());
  for (std::size_t i = 1; i < parts.size(); ++i) {
    const std::size_t equals = parts[i].find('=');
    if (equals == std::string::npos) fail("+%s=%s: %s is not <field>=<value>", plusarg, spec, parts[i].c_str());
    const std::string name = parts[i].substr(0, equals), text = parts[i].substr(equals + 1);
    const FieldInfo* f = std::find_if(event->fields, event->fields + event->field_count,
                                      [&](const FieldInfo& f) { return name == f.name; });
    if (f == event->fields + event->field_count)
      fail("+%s=%s: event %s has no field %s", plusarg, spec, event->name, name.c_str());
    uint64_t value;
    const EnumVariant* variant = std::find_if(f->variants, f->variants + f->variant_count,
                                              [&](const EnumVariant& v) { return text == v.name; });
    if (variant != f->variants + f->variant_count) value = variant->value;
    else if (!number(text, f->is_signed, value))
      fail("+%s=%s: %s is not %s of field %s", plusarg, spec, text.c_str(), f->variants ? "a variant" : "a value",
           name.c_str());
    // The trace ZigZag-encodes a signed value, so the trigger compares encoded values.
    if (f->is_signed) value = (value << 1) ^ static_cast<uint64_t>(static_cast<int64_t>(value) >> 63);
    t.fields.push_back({static_cast<unsigned>(f - event->fields), value});
  }
  return t;
}

void Runtime::violation(unsigned site, const char* format, ...) const {
  if (out_) std::fflush(out_);
  std::fprintf(stderr, "eventtrace: %s at site %u (cycle %lld): ", site_name(site), site, cycle_);
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
