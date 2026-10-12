// EventTrace runtime: turns cycle-stamped records from any transport into an ordered stream of numbered events.
// It depends on no simulator; an adapter feeds it, and the frontend's generated header supplies the sites.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace eventtrace {

class Runtime;

// The generated half of the runtime, as sites::Tracker: the site table, a handler per site, and the tracked
// objects and collections.
class Sites {
 public:
  virtual ~Sites() = default;
  virtual unsigned site_count() const = 0;
  virtual unsigned payload_words(unsigned site) const = 0;
  virtual const char* site_name(unsigned site) const = 0;
  virtual unsigned object_type_count() const = 0;

  // SHA-256 of the design's manifest, in hex, which the trace's header carries.
  virtual const char* manifest_sha256() const = 0;

  // Handles one record of the cycle being flushed: resolves its queries against the state before the clock edge,
  // stages its effects, and emits its events.
  virtual void handle(Runtime& rt, unsigned site, const uint32_t* words) = 0;

  // Applies the effects staged during the cycle, once all its records are handled.
  virtual void commit(Runtime& rt) = 0;
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
  void open(const char* path);

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

  const char* site_name(unsigned site) const { return sites_.site_name(site); }

  // Stops at a record that breaks a rule of the tracked model, naming its event, site, and cycle. Records of
  // earlier cycles are already out, and continuing would need a guess about which effect wins.
  [[noreturn]] void violation(unsigned site, const char* format, ...) const;

 private:
  struct Record {
    unsigned site;
    std::size_t offset;  // of its payload in words_
  };

  void flush();
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
};

}  // namespace eventtrace
