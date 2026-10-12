// EventTrace runtime: turns cycle-stamped records from any transport into an ordered stream of numbered events.
// It depends on no simulator; an adapter feeds it, and the frontend's generated header supplies the sites.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace eventtrace {

class Runtime;

// What the generated half says about each site's event, by which recording triggers name events, fields, and enum
// variants.
struct EnumVariant {
  const char* name;
  uint64_t value;
};

struct FieldInfo {
  const char* name;
  bool is_signed;               // ZigZag-encoded in the trace
  const EnumVariant* variants;  // an enum's variants, or null
  unsigned variant_count;
};

struct EventInfo {
  const char* name;         // as the manifest names it, such as instruction.decode
  const FieldInfo* fields;  // in the manifest's order, or null if it has none
  unsigned field_count;
};

// The generated half of the runtime, as sites::Tracker: the site table, a handler per site, and the tracked
// objects and collections.
class Sites {
 public:
  virtual ~Sites() = default;
  virtual unsigned site_count() const = 0;
  virtual unsigned payload_words(unsigned site) const = 0;
  // A site's event: its name, and its fields in the manifest's order.
  virtual const EventInfo& event(unsigned site) const = 0;
  virtual unsigned object_type_count() const = 0;

  // SHA-256 of the design's manifest, in hex, which the trace's header carries.
  virtual const char* manifest_sha256() const = 0;

  // Handles one record of the cycle being flushed: resolves its queries against the state before the clock edge,
  // stages its effects, and emits its events.
  virtual void handle(Runtime& rt, unsigned site, const uint32_t* words) = 0;

  // Applies the effects staged during the cycle, once all its records are handled.
  virtual void commit(Runtime& rt) = 0;
};

// What opens or closes the recording window, as +eventtrace_start or +eventtrace_stop gives it: `cycle:<n>` fires at
// cycle n, and `event:<name>[,<field>=<value>...]` at the first event of that name whose listed fields hold those
// values.
struct Trigger {
  enum class Mode { None, Cycle, Event };
  Mode mode = Mode::None;
  std::string spec;   // as given, for messages
  long long cycle = 0;
  std::vector<bool> sites;                             // the sites emitting the event, by site ID
  std::vector<std::pair<unsigned, uint64_t>> fields;  // each listed field's position, and its value as encoded
  bool hit = false;   // whether an event of the cycle being handled matched

  // Whether an event matches, given its present fields' values as packed varints and its mask of absent fields.
  bool matches(unsigned site, const std::string& values, uint64_t absent) const;
};

// Writes the trace as eventtrace.proto describes, encoding protobuf's wire format by hand so that simulators link no
// protobuf library. Handlers write each event they emit: its site, its subject if it has one, the values of its
// present fields in the manifest's order, and the positions of its absent fields.
class TraceWriter {
 public:
  void begin(unsigned site);
  void subject(unsigned long long id) { subject_ = id; }
  void value(uint64_t v) { put_varint(values_, v); }
  void value_signed(int64_t v) { value((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63)); }
  void absent(unsigned field) { absent_ |= uint64_t{1} << field; }
  void end();

 private:
  friend class Runtime;

  // The header Record, framed.
  static std::string header(const char* manifest_sha256);

  // The Record of a cycle's events, framed, or nothing if the cycle had none; then starts the next cycle.
  std::string take_cycle(long long cycle);

  static void put_varint(std::string& s, uint64_t v);
  static void put_message(std::string& s, unsigned field, const std::string& message);

  std::vector<Trigger*> watching_;  // event triggers that each event is matched against
  unsigned site_ = 0;
  unsigned long long subject_ = 0;
  uint64_t absent_ = 0;
  std::string values_;  // packed varints
  std::string events_;  // the cycle's Event fields so far
};

// Takes each record of a cycle in any order. Once the cycle is complete, it handles them in site-ID order, which
// is elaboration order, and numbers events and new objects as it goes, so their IDs and the output order never
// depend on the order in which a simulator delivered the records.
class Runtime {
 public:
  explicit Runtime(Sites& sites);

  // Sends records to the file at `path`. Each sink calls it at the start of the simulation with the path given as
  // +eventtrace=<path>; the first call opens the file, and later calls do nothing. Until a file is open, the runtime
  // ignores every record, so a simulation run without the plusarg pays only for the calls that deliver them.
  //
  // `start` and `stop`, from +eventtrace_start and +eventtrace_stop, are the triggers of the recording window, or
  // empty to record from the first cycle or to the last. Records are handled from the first cycle either way; only
  // the window's cycles are written.
  void open(const char* path, const char* start = "", const char* stop = "");

  // Where handlers write the events they emit.
  TraceWriter& writer() { return writer_; }

  // Takes one record whose payload is `words` 32-bit words. The payload is copied, since the caller owns it only
  // during the call. A cycle is complete once a record from a later cycle arrives or finish() is called.
  void accept(long long cycle, unsigned site, const uint32_t* payload, unsigned words);

  // Handles the last cycle. Later calls do nothing, so every sink may call it when the simulation ends.
  void finish();

  // The cycle being handled.
  long long cycle() const { return cycle_; }

  // The next event ID; handlers take one per record they emit.
  unsigned long long next_event_id() { return next_event_id_++; }

  // The next ID of an object type: each type counts from 0. Handlers call it for create events.
  unsigned long long create(unsigned object_type) { return next_object_ids_.at(object_type)++; }

  const char* site_name(unsigned site) const { return sites_.event(site).name; }

  // Stops at a record that breaks a rule of the tracked model, naming its event, site, and cycle. Records of
  // earlier cycles are already out, and continuing would need a guess about which effect wins.
  [[noreturn]] void violation(unsigned site, const char* format, ...) const;

 private:
  struct Record {
    unsigned site;
    std::size_t offset;  // of its payload in words_
  };

  void flush();
  Trigger parse_trigger(const char* plusarg, const char* spec) const;
  void watch();
  [[noreturn]] void fail(const char* format, ...) const;

  Sites& sites_;
  std::FILE* out_ = nullptr;  // null until open()
  TraceWriter writer_;
  long long cycle_ = -1;  // the cycle being buffered; -1 before the first record
  std::vector<Record> records_;
  std::vector<uint32_t> words_;
  unsigned long long next_event_id_ = 0;
  std::vector<unsigned long long> next_object_ids_;
  bool finished_ = false;

  // The recording window: it opens at the cycle where start_ fires, and closes after the one where stop_ fires,
  // counting only events from the cycle it opened.
  enum class Window { Waiting, Open, Closed };
  Trigger start_, stop_;
  Window window_ = Window::Open;
};

}  // namespace eventtrace
