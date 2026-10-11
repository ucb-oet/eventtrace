// EventTrace runtime: turns cycle-stamped records from any transport into an ordered stream of numbered events.
// It depends on no simulator; an adapter feeds it, and the frontend's generated header supplies the sites.
#pragma once

#include <cstddef>
#include <cstdint>
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

  // Handles one record of the cycle being flushed: resolves its queries against the state before the clock edge,
  // stages its effects, and emits its events.
  virtual void handle(Runtime& rt, unsigned site, const uint32_t* words) = 0;

  // Applies the effects staged during the cycle, once all its records are handled.
  virtual void commit(Runtime& rt) = 0;
};

// Takes each record of a cycle in any order. Once the cycle is complete, it handles them in site-ID order, which
// is elaboration order, and numbers events and new objects as it goes, so their IDs and the output order never
// depend on the order in which a simulator delivered the records.
class Runtime {
 public:
  explicit Runtime(Sites& sites);

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
  long long cycle_ = -1;  // the cycle being buffered; -1 before the first record
  std::vector<Record> records_;
  std::vector<uint32_t> words_;
  unsigned long long next_event_id_ = 0;
  std::vector<unsigned long long> next_object_ids_;
  bool finished_ = false;
};

}  // namespace eventtrace
