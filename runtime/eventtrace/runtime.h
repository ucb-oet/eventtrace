// EventTrace runtime: turns cycle-stamped records from any transport into an ordered stream of numbered events.
// It depends on no simulator; an adapter feeds it, and the frontend's generated header supplies the site table.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace eventtrace {

// Takes each record of a cycle in any order. Once the cycle is complete, it handles them in site-ID order, which
// is elaboration order, and numbers events and new objects as it goes, so their IDs and the output order never
// depend on the order in which a simulator delivered the records.
class Runtime {
 public:
  // Decodes and handles one record, returning false for an unknown site; generated as sites::dispatch.
  using Handler = bool (*)(Runtime& rt, long long cycle, unsigned long long event_id, unsigned site,
                           const uint32_t* words);

  // site_words[s] is site s's payload word count, for site_count sites; generated as sites::kPayloadWords and
  // sites::kSiteCount. object_types is generated as sites::kObjectTypeCount.
  Runtime(Handler handler, const unsigned* site_words, unsigned site_count, unsigned object_types);

  // Takes one record whose payload is `words` 32-bit words. The payload is copied, since the caller owns it only
  // during the call. A cycle is complete once a record from a later cycle arrives or finish() is called.
  void accept(long long cycle, unsigned site, const uint32_t* payload, unsigned words);

  // Handles the last cycle. Later calls do nothing, so every sink may call it when the simulation ends.
  void finish();

  // The next ID of an object type: each type counts from 0. Handlers call it for create events.
  unsigned long long create(unsigned object_type) { return next_object_ids_.at(object_type)++; }

 private:
  struct Record {
    unsigned site;
    std::size_t offset;  // of its payload in words_
  };

  void flush();
  [[noreturn]] void fail(const char* format, ...) const;

  Handler handler_;
  const unsigned* site_words_;
  unsigned site_count_;
  long long cycle_ = -1;  // the cycle being buffered; -1 before the first record
  std::vector<Record> records_;
  std::vector<uint32_t> words_;
  unsigned long long next_event_id_ = 0;
  std::vector<unsigned long long> next_object_ids_;
  bool finished_ = false;
};

}  // namespace eventtrace
