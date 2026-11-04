// oet's implementation: the library's bookkeeping, emit's checks, the declarations' checks when the trace opens, the
// manifest, and the entry points that `OET_SETUP imports.
#include "oet.h"

#include <array>
#include <bit>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

namespace oet {
namespace detail {

namespace {

// `s` without leading or trailing whitespace.
std::string trim(const std::string& s) {
  const std::size_t from = s.find_first_not_of(" \t\n\r");
  if (from == std::string::npos) return "";
  return s.substr(from, s.find_last_not_of(" \t\n\r") - from + 1);
}

// Whether `s` is an identifier: a letter or underscore, then letters, digits, and underscores.
bool is_identifier(const std::string& s) {
  if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

// Every record already has these, so no field may take their names.
const std::set<std::string> kReservedFieldNames = {"cycle", "event_id", "site_id", "subject"};

// A kind as the manifest's "type" names it.
const char* kind_name(Kind k) {
  switch (k) {
    case Kind::Bool: return "Bool";
    case Kind::UInt: return "UInt";
    case Kind::SInt: return "SInt";
    case Kind::Enum: return "Enum";
    case Kind::Id: return "Id";
  }
  return "?";
}

// A type as messages write it, such as Bool, UInt<8>, Id<instruction>, or an enum's name.
std::string type_text(const Type& t) {
  switch (t.kind) {
    case Kind::Bool: return "Bool";
    case Kind::Enum: return t.enum_decl()->name;
    case Kind::Id: return "Id<" + t.object().name + ">";
    default: return std::string(kind_name(t.kind)) + "<" + std::to_string(t.width) + ">";
  }
}

// A value's bits in decimal, signed for an SInt, for messages.
std::string value_text(const Encoded& e, const Type& t) {
  return t.kind == Kind::SInt ? std::to_string(static_cast<int64_t>(e.bits)) : std::to_string(e.bits);
}

// As ManifestGen quotes strings.
std::string quote(const std::string& s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (static_cast<unsigned char>(c) < ' ') {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", c);
      out += buf;
    } else out += c;
  }
  return out + "\"";
}

// SHA-256 of `data`, in hex, which the trace's header carries.
std::string sha256(const std::string& data) {
  static constexpr std::array<uint32_t, 64> k = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
      0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
      0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
      0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};
  std::array<uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::string msg = data;
  const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
  msg.push_back(static_cast<char>(0x80));
  while (msg.size() % 64 != 56) msg.push_back('\0');
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>(bits >> (8 * i)));
  for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
    std::array<uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i)
      for (int b = 0; b < 4; ++b) w[i] = w[i] << 8 | static_cast<unsigned char>(msg[chunk + 4 * i + b]);
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::array<uint32_t, 8> v = h;
    for (int i = 0; i < 64; ++i) {
      const uint32_t s1 = std::rotr(v[4], 6) ^ std::rotr(v[4], 11) ^ std::rotr(v[4], 25);
      const uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
      const uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
      const uint32_t s0 = std::rotr(v[0], 2) ^ std::rotr(v[0], 13) ^ std::rotr(v[0], 22);
      const uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
      const uint32_t t2 = s0 + maj;
      v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
    }
    for (int i = 0; i < 8; ++i) h[i] += v[i];
  }
  std::string hex;
  char buf[9];
  for (const uint32_t x : h) {
    std::snprintf(buf, sizeof buf, "%08x", x);
    hex += buf;
  }
  return hex;
}

}  // namespace

std::vector<std::string> split_names(const char* names) {
  std::vector<std::string> out;
  std::string current;
  for (const char* c = names;; ++c) {
    if (*c == ',' || *c == '\0') {
      std::string name = current.substr(0, current.find('='));
      out.push_back(trim(name));
      current.clear();
      if (*c == '\0') break;
    } else {
      current += *c;
    }
  }
  return out;
}

EnumDecl make_enum(const char* name, const char* names, std::initializer_list<uint64_t> values) {
  EnumDecl d;
  d.name = name;
  const std::vector<std::string> split = split_names(names);
  uint64_t largest = 0;
  std::size_t i = 0;
  for (const uint64_t v : values) {
    d.variants.push_back({split.at(i++), v});
    largest = std::max(largest, v);
  }
  d.width = std::max(1u, static_cast<unsigned>(std::bit_width(largest)));
  return d;
}

const ObjectType& register_object(ObjectType type) {
  Library& lib = library();
  type.index = static_cast<unsigned>(lib.types.size());
  lib.types.push_back(std::make_unique<ObjectType>(std::move(type)));
  return *lib.types.back();
}

std::string describe(const Object& o) { return std::string(type_name(o)) + "#" + std::to_string(o.id); }

long long current_cycle() { return library().runtime().cycle(); }

void touch(Object& o) {
  Library::Running* r = library().running;
  if (r && std::find(r->touched.begin(), r->touched.end(), &o) == r->touched.end()) r->touched.push_back(&o);
}

bool recording() { return library().recording(); }

void queue(long long cycle, CallSite where, std::function<void()> handler) {
  library().queue(cycle, where, std::move(handler));
}

void fail(const std::string& message) { library().fail(message); }

EventBase::EventBase(const char* n) : name(n) { library().events.push_back(this); }

std::string EventBase::full_name() const { return subject ? subject().name + "." + name : name; }

Object& EventBase::create_object(const ObjectType& type, Object& (*make)(unsigned long long id)) {
  Library& lib = library();
  if (!lib.running) fail(full_name() + ".create() is called outside a handler");
  if (lifecycle != Lifecycle::Create) fail(full_name() + ".create() is called, but the event is not declared oet::create");
  Object& o = make(lib.runtime().create(type.index));
  o.oet_type = &type;
  type.bind(o);
  o.oet_created = lib.runtime().cycle();
  o.oet_created_by = this;
  touch(o);
  return o;
}

void EventBase::emit_record(Object* subject_object, const void* value_struct, const Mode& mode,
                            const std::function<void(unsigned, unsigned long long)>& destroy) {
  Library& lib = library();
  if (!lib.running) fail(full_name() + " is emitted outside a handler");
  const long long cycle = lib.runtime().cycle();
  const std::string what = subject_object ? describe(*subject_object) : full_name();

  if (subject_object) {
    if (!subject_object->oet_type) fail(full_name() + " is emitted on an object the library did not create");
    if (lifecycle == Lifecycle::Create) {
      if (subject_object->oet_created_by != this || subject_object->oet_created != cycle ||
          subject_object->oet_create_emitted)
        fail(full_name() + " is emitted on " + what + ", which its create() did not make in this handler");
    } else if (subject_object->oet_created == cycle) {
      fail(full_name() + " is emitted on " + what + ", which was created in this cycle and exists from the next");
    }
  }

  const bool blocked = mode.kind == Mode::Kind::Blocked;
  if (blocked && lifecycle == Lifecycle::Abort)
    fail(full_name() + " aborts its subject, so a record of it is not blocked; name the cause with oet::by");
  if (mode.kind == Mode::Kind::AbortedBy && lifecycle != Lifecycle::Abort)
    fail(full_name() + " is emitted with oet::by, which names what aborted the subject of an abort event; use "
                       "oet::blocked to name what blocks it");
  if (mode.cause) {
    if (!cause) fail(full_name() + " names a cause, but declares no oet::by<T>()");
    if (mode.cause_type != cause)
      fail(full_name() + " names a " + mode.cause_type().name + " as the cause, but declares oet::by<" + cause().name +
           ">()");
  }
  const unsigned site = lib.site_of(*this, blocked);

  Library::Record record{site, subject_object ? subject_object->id : 0, {}, {}, 0};
  std::vector<bool> used(subject_object ? subject_object->oet_assigns.size() : 0, false);
  for (std::size_t f = 0; f < fields.size(); ++f) {
    const ManifestField& m = fields[f];
    Encoded e;
    switch (m.source) {
      case ManifestField::Source::Subject:
        for (std::size_t a = 0; a < subject_object->oet_assigns.size(); ++a)
          if (subject_object->oet_assigns[a].index == m.index) {
            e = subject_object->oet_assigns[a].value;
            used[a] = true;
          }
        break;
      case ManifestField::Source::Value: e = values[m.index].read(value_struct); break;
      case ManifestField::Source::Cause:
        if (mode.cause) e = {true, *mode.cause, true};
        break;
    }
    if (!e.set) {
      if (!m.optional)
        fail(what + ": " + full_name() + (m.source == ManifestField::Source::Subject ? " lists the field " : " has the value ") +
             m.name + ", which this record does not set");
      record.absent |= uint64_t{1} << f;
      continue;
    }
    if (!e.fits) fail(what + ": " + m.name + " = " + value_text(e, m.type) + " does not fit " + type_text(m.type));
    record.values.push_back(e.bits);
    record.is_signed.push_back(m.type.kind == Kind::SInt);
  }
  for (std::size_t a = 0; a < used.size(); ++a)
    if (!used[a])
      fail(what + "." + subject_object->oet_type->fields[subject_object->oet_assigns[a].index].name +
           " is assigned, but " + full_name() + " does not list it");

  const unsigned long long event = lib.runtime().next_event_id();
  if (subject_object) {
    for (const Object::PendingAssign& a : subject_object->oet_assigns) lib.stage_assign(site, event, *subject_object, a);
    for (const Move& move : subject_object->oet_moves) move(site, event);
    subject_object->oet_assigns.clear();
    subject_object->oet_moves.clear();
    if (lifecycle == Lifecycle::Complete || lifecycle == Lifecycle::Abort) destroy(site, event);
    if (lifecycle == Lifecycle::Create) subject_object->oet_create_emitted = true;
  }
  lib.records.push_back(std::move(record));
}

Library& Library::get() {
  // Never destroyed, so that collections and the exit handler can use it however late.
  static Library* const lib = new Library;
  return *lib;
}

void Library::fail(const std::string& message) const {
  const std::string full = context() + message;
  if (runtime_) runtime_->fail("%s", full.c_str());
  std::fprintf(stderr, "eventtrace: %s\n", full.c_str());
  std::abort();
}

std::string Library::context() const {
  if (!running) return "";
  const Call& call = *running->call;
  const std::string name = call.name + (call.count > 1 ? "[" + std::to_string(running->index) + "]" : "");
  const std::string cycle = std::to_string(runtime_->cycle());
  if (call.events.size() == 1)
    return call.events[0]->full_name() + " at site " + std::to_string(call.sites[running->index][0]) + " (call " +
           name + ", cycle " + cycle + "): ";
  return "call " + name + " (cycle " + cycle + "): ";
}

void Library::queue(long long cycle, CallSite where, std::function<void()> handler) {
  const Call& call = *where.call;
  if (call.count > 1 && !where.indexed)
    fail("call " + call.name + " has " + std::to_string(call.count) + " indices, so oet::at takes " + call.name +
         "_call[index]");
  if (where.index >= call.count)
    fail("call " + call.name + " has " + std::to_string(call.count) + " indices, but is given index " +
         std::to_string(where.index));
  const unsigned slot = call.first_slot + where.index;
  if (last_cycle_[slot] == cycle)
    fail("call " + call.name + (call.count > 1 ? "[" + std::to_string(where.index) + "]" : "") +
         " fires twice in cycle " + std::to_string(cycle));
  last_cycle_[slot] = cycle;
  runtime_->queue(cycle, slot, [this, c = &call, index = where.index, h = std::move(handler)] {
    Running r{c, index, {}};
    running = &r;
    h();
    for (Object* o : r.touched) {
      if (!o->oet_assigns.empty() || !o->oet_moves.empty())
        fail(describe(*o) + " has effects that no event emitted on it");
      if (o->oet_created_by && o->oet_created == runtime_->cycle() && !o->oet_create_emitted)
        fail(describe(*o) + " was created, but its create event was not emitted on it");
    }
    running = nullptr;
  });
}

unsigned Library::site_of(const EventBase& event, bool blocked) const {
  const Call& call = *running->call;
  for (std::size_t k = 0; k < call.events.size(); ++k)
    if (call.events[k] == &event) return call.sites[running->index][k] + (blocked ? 1 : 0);
  fail(event.full_name() + " is emitted, but call " + call.name + " does not declare it");
}

void Library::handle(eventtrace::Runtime&, unsigned site, const uint32_t*) {
  fail("a payload record from site " + std::to_string(site) + " reached the C++ library");
}

void Library::commit(eventtrace::Runtime& rt) {
  std::stable_sort(records.begin(), records.end(), [](const Record& a, const Record& b) { return a.site < b.site; });
  eventtrace::TraceWriter& w = rt.writer();
  for (const Record& r : records) {
    w.begin(r.site);
    w.subject(r.subject);
    for (std::size_t i = 0; i < r.values.size(); ++i) {
      if (r.is_signed[i]) w.value_signed(static_cast<int64_t>(r.values[i]));
      else w.value(r.values[i]);
    }
    for (unsigned f = 0; f < 64; ++f)
      if (r.absent >> f & 1) w.absent(f);
    w.end();
  }
  records.clear();
  apply(rt);
}

void Library::open(const char* path, const char* start, const char* stop) {
  if (runtime_) return;
  finalize();
  runtime_.emplace(*this);
  runtime_->open(path, start, stop);
  const std::string manifest_path = std::string(path) + ".json";
  std::ofstream out(manifest_path, std::ios::binary);
  if (!(out << manifest_)) fail("cannot write the manifest to " + manifest_path);
  // A simulation that ends through $stop or $fatal skips the final blocks, so the trace also finishes at exit.
  std::atexit([] { library().finish(); });
}

void Library::finish() {
  if (runtime_) runtime_->finish();
}

void Library::finalize() {
  // Every object type that a field, value, or cause names, so that each has its ID counter.
  for (EventBase* e : events) {
    if (e->subject) e->subject();
    if (e->cause) e->cause();
    for (const ValueDecl& v : e->values)
      if (v.type.kind == Kind::Id) v.type.object();
  }
  for (std::size_t i = 0; i < types.size(); ++i)
    for (const FieldDecl& f : types[i]->fields)
      if (f.type.kind == Kind::Id) f.type.object();

  // The events with sites, sorted by name, as the manifest lists them.
  std::vector<EventBase*> used;
  for (const Call* call : calls)
    for (EventBase* e : call->events)
      if (std::find(used.begin(), used.end(), e) == used.end()) used.push_back(e);
  std::stable_sort(used.begin(), used.end(),
                   [](const EventBase* a, const EventBase* b) { return a->full_name() < b->full_name(); });
  for (std::size_t i = 1; i < used.size(); ++i)
    if (used[i]->full_name() == used[i - 1]->full_name())
      fail("two oet::Event declarations are named " + used[i]->full_name() +
           "; sites sharing an event share its declaration");
  manifest_events_ = used;

  // Each event's fields, sorted by name: the subject fields it lists, its values, and its cause.
  for (EventBase* e : manifest_events_) {
    e->fields.clear();
    for (const EventBase::Listed& l : e->listed) {
      if (l.index < 0) fail(e->full_name() + " lists a field that its subject's OET_OBJECT does not");
      const FieldDecl& f = e->subject().fields[static_cast<unsigned>(l.index)];
      e->fields.push_back({f.name, f.type, l.optional, nullptr, EventBase::ManifestField::Source::Subject,
                           static_cast<unsigned>(l.index)});
    }
    for (std::size_t v = 0; v < e->values.size(); ++v)
      e->fields.push_back({e->values[v].name, e->values[v].type, e->values[v].optional, nullptr,
                           EventBase::ManifestField::Source::Value, static_cast<unsigned>(v)});
    if (e->cause)
      e->fields.push_back({"by", Type{Kind::Id, 0, nullptr, e->cause}, true,
                           e->lifecycle == Lifecycle::Abort ? "aborted_by" : "blocked_by",
                           EventBase::ManifestField::Source::Cause, 0});
    std::stable_sort(e->fields.begin(), e->fields.end(),
                     [](const auto& a, const auto& b) { return a.name < b.name; });
  }
  check_declarations();

  // Sites: for each call, each index, and each event it declares, in that order, a non-blocking site and, for an
  // event with a subject, a blocking one after it.
  site_events_.clear();
  unsigned slot = 0;
  for (Call* call : calls) {
    call->first_slot = slot;
    slot += call->count;
    call->sites.assign(call->count, {});
    for (unsigned index = 0; index < call->count; ++index)
      for (EventBase* e : call->events) {
        const unsigned event =
            static_cast<unsigned>(std::find(manifest_events_.begin(), manifest_events_.end(), e) - manifest_events_.begin());
        call->sites[index].push_back(static_cast<unsigned>(site_events_.size()));
        site_events_.push_back({event, false});
        if (e->subject) site_events_.push_back({event, true});
      }
  }
  last_cycle_.assign(slot, -1);

  // What the recording window's event triggers read.
  infos_.clear();
  field_infos_.assign(manifest_events_.size(), {});
  variant_infos_.clear();
  std::size_t variant_lists = 0;
  for (const EventBase* e : manifest_events_)
    for (const auto& f : e->fields) variant_lists += f.type.kind == Kind::Enum;
  variant_infos_.reserve(variant_lists);
  names_.clear();
  names_.reserve(manifest_events_.size());
  for (std::size_t i = 0; i < manifest_events_.size(); ++i) {
    const EventBase* e = manifest_events_[i];
    names_.push_back(e->full_name());
    for (const auto& f : e->fields) {
      const eventtrace::EnumVariant* variants = nullptr;
      unsigned count = 0;
      if (f.type.kind == Kind::Enum) {
        variant_infos_.emplace_back();
        for (const auto& [name, value] : f.type.enum_decl()->variants) variant_infos_.back().push_back({name.c_str(), value});
        variants = variant_infos_.back().data();
        count = static_cast<unsigned>(variant_infos_.back().size());
      }
      field_infos_[i].push_back({f.name.c_str(), f.type.kind == Kind::SInt, variants, count});
    }
    infos_.push_back({names_.back().c_str(), field_infos_[i].empty() ? nullptr : field_infos_[i].data(),
                      static_cast<unsigned>(field_infos_[i].size())});
  }

  manifest_ = build_manifest();
  sha256_ = sha256(manifest_);
}

void Library::check_declarations() const {
  // Stops the run unless `name`, which `what` describes, is an identifier and not a reserved name.
  const auto field_name = [&](const std::string& what, const std::string& name) {
    if (!is_identifier(name)) fail(what + ": " + quote(name) + " is not an identifier");
    if (kReservedFieldNames.count(name))
      fail(what + " uses a reserved name; every record already has cycle, event_id, site_id, subject");
  };
  std::map<std::string, const EnumDecl*> enums;
  // Checks the enum that `t` names, if any: its name is an identifier, and no other enum has it.
  const auto enum_of = [&](const Type& t) {
    if (t.kind != Kind::Enum) return;
    const EnumDecl* d = t.enum_decl();
    if (!is_identifier(d->name)) fail("enum " + quote(d->name) + ": the name is not an identifier");
    const auto [it, inserted] = enums.insert({d->name, d});
    if (!inserted && it->second != d) fail("two enums are named " + d->name + "; enum names are unique");
  };
  for (const auto& t : types) {
    if (!is_identifier(t->name)) fail("object type " + quote(t->name) + ": the name is not an identifier");
    if (t->fields.size() > 64)
      fail("object type " + t->name + " declares " + std::to_string(t->fields.size()) +
           " fields; an object type has at most 64");
    unsigned parents = 0;
    for (const FieldDecl& f : t->fields) {
      field_name("field " + t->name + "." + f.name, f.name);
      parents += f.parent;
      enum_of(f.type);
    }
    if (parents > 1) fail("object type " + t->name + " declares " + std::to_string(parents) + " parents; a type has at most one");
  }
  for (const EventBase* e : manifest_events_) {
    const std::string what = "event " + e->full_name();
    if (!is_identifier(e->name)) fail(what + ": the name is not an identifier");
    if (e->lifecycle != Lifecycle::None && !e->subject) fail(what + " has a lifecycle but no subject");
    if (e->cause && !e->subject) fail(what + " declares oet::by<T>() but has no subject to block or abort");
    if (e->fields.size() > 64)
      fail(what + " has " + std::to_string(e->fields.size()) + " fields; an event has at most 64");
    for (std::size_t i = 0; i < e->fields.size(); ++i) {
      if (e->fields[i].source != EventBase::ManifestField::Source::Cause) field_name(what + "'s field " + e->fields[i].name, e->fields[i].name);
      if (i > 0 && e->fields[i].name == e->fields[i - 1].name) fail(what + " has two fields named " + e->fields[i].name);
      enum_of(e->fields[i].type);
    }
    if (e->subject) {
      const ObjectType& s = e->subject();
      for (std::size_t f = 0; f < s.fields.size(); ++f) {
        const auto listed = std::find_if(e->listed.begin(), e->listed.end(),
                                         [&](const EventBase::Listed& l) { return l.index == static_cast<int>(f); });
        const bool is_const = s.fields[f].property == Property::Const;
        if (e->lifecycle == Lifecycle::Create && is_const && (listed == e->listed.end() || listed->optional))
          fail(what + " creates its subject, so it lists the const field " + s.fields[f].name + ", and not as optional");
        if (e->lifecycle != Lifecycle::Create && is_const && listed != e->listed.end())
          fail(what + " lists the const field " + s.fields[f].name + ", which only a create event assigns");
      }
    }
  }
  for (const Call* call : calls) {
    if (!is_identifier(call->name)) fail("call " + quote(call->name) + ": the name is not an identifier");
    if (call->count == 0) fail("call " + call->name + " has no indices");
  }
  for (const CollectionDecl& c : collections) {
    const std::string entries = std::to_string(c.entries.value_or(0));
    switch (c.kind) {
      case CollectionDecl::Kind::Set: break;
      case CollectionDecl::Kind::Queue:
        if (c.entries && *c.entries == 0) fail("queue " + c.name + " has 0 entries; a queue has at least 1");
        break;
      case CollectionDecl::Kind::Rob:
        if (*c.entries == 0) fail("ROB " + c.name + " has 0 entries; a ROB has at least 1");
        if (c.keys < *c.entries)
          fail("ROB " + c.name + " has " + std::to_string(c.keys) + " keys for " + entries +
               " entries; it needs a key for each entry");
        break;
      case CollectionDecl::Kind::Map:
        if (*c.entries == 0) fail("map " + c.name + " has 0 entries; a map has at least 1");
        break;
    }
  }
}

std::string Library::build_manifest() const {
  // As ManifestGen writes it: the same layout, so that the two flows' manifests compare line by line.
  // One field's entry: its name, its type, the type's object or its enum and width, and then `more`.
  const auto field = [](const std::string& name, const Type& t, const std::string& more) {
    std::string detail;
    if (t.kind == Kind::Id) detail = ", \"object\": " + quote(t.object().name);
    else {
      if (t.kind == Kind::Enum) detail = ", \"enum\": " + quote(t.enum_decl()->name);
      detail += ", \"width\": " + std::to_string(t.width);
    }
    return "        {\"name\": " + quote(name) + ", \"type\": \"" + kind_name(t.kind) + "\"" + detail + more + "}";
  };
  // `items` as a JSON list, one per line, indented as ManifestGen's are.
  const auto list = [](const std::vector<std::string>& items) {
    if (items.empty()) return std::string("[]");
    std::string s = "[\n";
    for (std::size_t i = 0; i < items.size(); ++i) s += items[i] + (i + 1 < items.size() ? ",\n" : "\n");
    return s + "      ]";
  };
  // `items`, one per line, separated by commas.
  const auto join = [](const std::vector<std::string>& items) {
    std::string s;
    for (std::size_t i = 0; i < items.size(); ++i) s += items[i] + (i + 1 < items.size() ? ",\n" : "");
    return s;
  };
  std::vector<std::string> entries;
  std::map<std::string, const EnumDecl*> enums;
  for (const EventBase* e : manifest_events_) {
    std::vector<std::string> fields;
    for (const auto& f : e->fields) {
      std::string more = f.optional ? ", \"optional\": true" : "";
      if (f.role) more += std::string(", \"role\": \"") + f.role + "\"";
      fields.push_back(field(f.name, f.type, more));
      if (f.type.kind == Kind::Enum) enums[f.type.enum_decl()->name] = f.type.enum_decl();
    }
    std::string subject = e->subject ? "\n      \"subject\": " + quote(e->subject().name) + "," : "";
    std::string lifecycle;
    if (e->lifecycle == Lifecycle::Create) lifecycle = "\n      \"lifecycle\": \"create\",";
    if (e->lifecycle == Lifecycle::Complete) lifecycle = "\n      \"lifecycle\": \"complete\",";
    if (e->lifecycle == Lifecycle::Abort) lifecycle = "\n      \"lifecycle\": \"abort\",";
    entries.push_back("    {\n      \"name\": " + quote(e->full_name()) + "," + subject + lifecycle +
                      "\n      \"fields\": " + list(fields) + "\n    }");
  }
  std::vector<const ObjectType*> sorted_types;
  for (const auto& t : types) sorted_types.push_back(t.get());
  std::stable_sort(sorted_types.begin(), sorted_types.end(),
                   [](const ObjectType* a, const ObjectType* b) { return a->name < b->name; });
  std::vector<std::string> object_entries;
  for (const ObjectType* t : sorted_types) {
    std::vector<std::string> fields;
    for (const FieldDecl& f : t->fields) {
      const char* property = f.property == Property::Const ? "const" : f.property == Property::SetOnce ? "setOnce" : "mutable";
      fields.push_back(field(f.name, f.type, std::string(", \"property\": \"") + property + "\"" +
                                                 (f.parent ? ", \"role\": \"parent\"" : "")));
      if (f.type.kind == Kind::Enum) enums[f.type.enum_decl()->name] = f.type.enum_decl();
    }
    object_entries.push_back("    {\n      \"name\": " + quote(t->name) + ",\n      \"fields\": " + list(fields) + "\n    }");
  }
  std::vector<std::string> enum_entries;
  for (const auto& [name, d] : enums) {
    std::string values;
    for (std::size_t i = 0; i < d->variants.size(); ++i)
      values += (i ? ", " : "") + quote(d->variants[i].first) + ": " + std::to_string(d->variants[i].second);
    enum_entries.push_back("    {\"name\": " + quote(name) + ", \"values\": {" + values + "}}");
  }
  std::vector<std::string> site_entries;
  for (std::size_t s = 0; s < site_events_.size(); ++s)
    site_entries.push_back("    {\"site_id\": " + std::to_string(s) + ", \"event_name\": " +
                           quote(manifest_events_[site_events_[s].event]->full_name()) +
                           (site_events_[s].blocks ? ", \"blocks\": true" : "") + "}");
  return "{\n  \"events\": [\n" + join(entries) + "\n  ],\n  \"objects\": [\n" + join(object_entries) +
         "\n  ],\n  \"enums\": [\n" + join(enum_entries) + "\n  ],\n  \"sites\": [\n" + join(site_entries) +
         "\n  ]\n}\n";
}

}  // namespace detail

void fail(const std::string& message) { detail::fail(message); }

Call::Call(const char* n, Events list) : Call(n, 1, list) {}

Call::Call(const char* n, unsigned c, Events list) : name(n), count(c) {
  for (detail::EventBase& e : list) events.push_back(&e);
  detail::library().calls.push_back(this);
}

}  // namespace oet

// The SystemVerilog side's entry points, which `OET_SETUP imports: the first opens the trace given as
// +eventtrace=<path>, with the recording window's triggers, and the manifest beside it as <path>.json.
extern "C" void oet_open(const char* path, const char* start, const char* stop) {
  oet::detail::library().open(path, start, stop);
}

// Finishes the trace, from `OET_SETUP's final block.
extern "C" void oet_finish() { oet::detail::library().finish(); }
