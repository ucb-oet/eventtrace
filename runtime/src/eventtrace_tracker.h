// EventTrace object tracking: what a generated sites::Tracker is built from. Objects live in stores, collections
// keep their members in join order, queries scan them, and effects are staged during a cycle, checked against each
// other, and applied when it ends. A collection is a set, a queue, a ROB, or a map, and each kind checks the
// discipline its operations imply.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "eventtrace_runtime.h"

namespace eventtrace {

// What the runtime keeps for every live object besides its fields, which a generated subclass declares along with
// its type name, kTypeName.
struct Object {
  unsigned long long id = 0;
  std::vector<unsigned> member_of;  // the collections holding it, by index

  // Effects on the object in the cycle `effect_cycle`, for the checks between events of one cycle. Each event
  // stages all its effects at once, so the events touching an object arrive one after another.
  long long effect_cycle = -1;
  unsigned long long last_event = 0;  // the latest event with an effect on it
  unsigned last_site = 0;
  unsigned earlier_site = 0;          // the site of the event before it, when several events have effects
  bool several = false;
  bool destroyed = false;             // by the event destroy_site emitted
  unsigned destroy_site = 0;
  uint64_t assigned = 0;              // the fields assigned, one bit each
};

// An object's type name in messages: a generated object type's kTypeName. Another kind of object, such as the C++
// library's, gives its own through an overload that argument-dependent lookup finds.
template <class T>
auto type_name(const T&) -> decltype(T::kTypeName) {
  return T::kTypeName;
}

// The live objects of one type, by ID.
template <class T>
class Store {
 public:
  T& create(unsigned long long id) {
    T& o = objects_[id];
    o.id = id;
    return o;
  }
  void erase(unsigned long long id) { objects_.erase(id); }

 private:
  std::unordered_map<unsigned long long, T> objects_;  // references stay valid as other objects come and go
};

class Effects;

// What applying a cycle's membership changes needs from a collection of any kind.
class CollectionBase {
 public:
  CollectionBase(unsigned index, const char* name) : index_(index), name_(name) {}
  virtual ~CollectionBase() = default;
  unsigned index() const { return index_; }
  const char* name() const { return name_; }

  // Applies the cycle's staged removals, then its additions, which join in the order they were staged, and checks
  // what the collection's kind requires of the result.
  virtual void apply(Runtime& rt) = 0;

 protected:
  // Records that o joined or left this collection.
  void joined(Object& o) const { o.member_of.push_back(index_); }
  void left(Object& o) const { o.member_of.erase(std::find(o.member_of.begin(), o.member_of.end(), index_)); }

 private:
  unsigned index_;
  const char* name_;
};

// A set of objects that records the order in which its members joined.
template <class T>
class Collection final : public CollectionBase {
 public:
  Collection(Effects& effects, unsigned index, const char* name);

  // The members before the clock edge, in join order.
  const std::vector<T*>& members() const { return members_; }

  void apply(Runtime&) override {
    for (T* o : removes_) {
      members_.erase(std::find(members_.begin(), members_.end(), o));
      left(*o);
    }
    for (T* o : adds_) {
      members_.push_back(o);
      joined(*o);
    }
    removes_.clear();
    adds_.clear();
  }

 private:
  friend class Effects;
  std::vector<T*> members_, adds_, removes_;
};

// A change staged on a queue, ROB, or map, with the site that made it, which a check at the end of the cycle names.
template <class T>
struct Staged {
  T* object;
  unsigned site;
  uint64_t key;
};

// A FIFO: members join at the tail and leave from the head, through pop, all at once, through clear, or from the
// tail, through dropYoungest. With `entries` nonzero, it may hold at most that many members at the end of a cycle.
template <class T>
class Queue : public CollectionBase {
 public:
  Queue(Effects& effects, unsigned index, const char* name, std::size_t entries);

  // The members before the clock edge, oldest first.
  const std::vector<T*>& members() const { return members_; }

  // The `k` youngest members, for dropYoungest(k); a violation if there are fewer.
  std::vector<T*> youngest(Runtime& rt, unsigned site, uint64_t k) const {
    if (k > members_.size())
      rt.violation(site, "dropYoungest(%llu) found only %zu members of %s", static_cast<unsigned long long>(k),
                   members_.size(), name());
    return std::vector<T*>(members_.end() - static_cast<std::ptrdiff_t>(k), members_.end());
  }

  void apply(Runtime& rt) override {
    leave(rt);
    for (const Staged<T>& a : adds_) join(a.object, 0);
    check_entries(rt);
    adds_.clear();
  }

 protected:
  friend class Effects;

  // Checks that the cycle's pops took the first members, then removes every member that pop, rollback, clear, or
  // dropYoungest takes. Returns the key of the oldest member that rollback or clear removed, if there is one.
  std::optional<uint64_t> leave(Runtime& rt) {
    const std::size_t pops = pops_.size();
    for (const Staged<T>& p : pops_) {
      const std::size_t at = position(*p.object);
      if (at >= pops)
        rt.violation(p.site, "%s#%llu is popped from %s at position %zu, but this cycle's %zu pop%s must take its "
                     "first member%s", type_name(*p.object), p.object->id, name(), at, pops, pops == 1 ? "" : "s",
                     pops == 1 ? "" : "s");
    }
    std::vector<bool> gone(members_.size(), false);
    for (const Staged<T>& p : pops_) gone[position(*p.object)] = true;
    std::size_t oldest = members_.size();
    for (const Staged<T>& d : drops_) {
      const std::size_t at = position(*d.object);
      gone[at] = true;
      oldest = std::min(oldest, at);
    }
    std::optional<uint64_t> rewind;
    if (oldest < members_.size()) rewind = keys_[oldest];
    std::size_t kept = 0;
    for (std::size_t i = 0; i < members_.size(); ++i) {
      if (gone[i]) {
        left(*members_[i]);
        continue;
      }
      members_[kept] = members_[i];
      keys_[kept] = keys_[i];
      ++kept;
    }
    members_.resize(kept);
    keys_.resize(kept);
    pops_.clear();
    drops_.clear();
    return rewind;
  }

  void join(T* o, uint64_t key) {
    members_.push_back(o);
    keys_.push_back(key);
    joined(*o);
  }

  // Flags a queue holding more members than its entries, naming the cycle's last addition.
  void check_entries(Runtime& rt) const {
    if (entries_ != 0 && members_.size() > entries_ && !adds_.empty())
      rt.violation(adds_.back().site, "%s holds %zu members at the end of the cycle, more than its %zu entries", name(),
                   members_.size(), entries_);
  }

  std::size_t position(const T& o) const {
    return static_cast<std::size_t>(std::find(members_.begin(), members_.end(), &o) - members_.begin());
  }
  std::size_t position_of_key(uint64_t key) const {
    return static_cast<std::size_t>(std::find(keys_.begin(), keys_.end(), key) - keys_.begin());
  }

  // Whether o leaves this cycle, by pop, rollback, or clear.
  bool leaving(const T& o) const {
    const auto is = [&](const Staged<T>& s) { return s.object == &o; };
    return std::any_of(pops_.begin(), pops_.end(), is) || std::any_of(drops_.begin(), drops_.end(), is);
  }

  std::vector<T*> members_;
  std::vector<uint64_t> keys_;  // each member's key, for a ROB; 0 in a plain queue
  std::vector<Staged<T>> adds_, pops_, drops_;
  std::size_t entries_;
};

// A queue whose tail allocates keys, as a reorder buffer's tail allocates tags. Keys count modulo `keys`. Each
// addition gives the hardware's key, which must be the one the tail allocates next; the first sets where allocation
// starts. Removing members through rollback or clear moves the tail back to the oldest of them.
template <class T>
class Rob final : public Queue<T> {
 public:
  Rob(Effects& effects, unsigned index, const char* name, std::size_t entries, uint64_t keys)
      : Queue<T>(effects, index, name, entries), keys_count_(keys) {}

  // The live member with `key`, or null.
  T* find(uint64_t key) const {
    const std::size_t at = this->position_of_key(key);
    return at < this->members_.size() ? this->members_[at] : nullptr;
  }

  // The live member with `key`, for rob(key); a violation if there is none.
  T* lookup(Runtime& rt, unsigned site, uint64_t key) const {
    if (T* o = find(key)) return o;
    rt.violation(site, "%s has no member with key %llu", this->name(), static_cast<unsigned long long>(key));
  }

  // The members from key `tail` through the youngest, which a rollback to `tail` removes. None if `tail` is the key
  // the tail allocates next, or if no addition has set that yet; nothing at all if `tail` is neither that nor a live
  // key, which is not a place the tail can move back to.
  std::optional<std::vector<T*>> following(uint64_t tail) const {
    const std::size_t at = this->position_of_key(tail);
    if (at < this->members_.size())
      return std::vector<T*>(this->members_.begin() + static_cast<std::ptrdiff_t>(at), this->members_.end());
    if (!next_ || tail == *next_) return std::vector<T*>();
    return std::nullopt;
  }

  // The key the tail allocates next, once the first addition has set it.
  std::optional<uint64_t> next() const { return next_; }

  // The members a rollback to `tail` removes, for rollback(tail); a violation if `tail` is neither a live key nor the
  // tail.
  std::vector<T*> from(Runtime& rt, unsigned site, uint64_t tail) const {
    if (std::optional<std::vector<T*>> leaving = following(tail)) return *leaving;
    rt.violation(site, "rolls %s back to key %llu, which is neither a live key nor its tail, %llu", this->name(),
                 static_cast<unsigned long long>(tail), static_cast<unsigned long long>(*next_));
  }

  void apply(Runtime& rt) override {
    if (const std::optional<uint64_t> rewind = this->leave(rt)) next_ = *rewind;
    for (const Staged<T>& a : this->adds_) {
      const auto key = static_cast<unsigned long long>(a.key);
      if (a.key >= keys_count_)
        rt.violation(a.site, "%s#%llu joins %s with key %llu, outside its %llu keys", type_name(*a.object),
                     a.object->id, this->name(), key, static_cast<unsigned long long>(keys_count_));
      if (!next_) next_ = a.key;
      if (a.key != *next_)
        rt.violation(a.site, "%s#%llu joins %s with key %llu, but its tail allocates %llu next", type_name(*a.object),
                     a.object->id, this->name(), key, static_cast<unsigned long long>(*next_));
      this->join(a.object, a.key);
      next_ = (a.key + 1) % keys_count_;
    }
    this->check_entries(rt);
    this->adds_.clear();
  }

 private:
  uint64_t keys_count_;
  std::optional<uint64_t> next_;  // the key the tail allocates next, once the first addition has set it
};

// Members under keys the hardware chooses, as from a free list, each below `entries`. It keeps join order, which its
// views follow.
template <class T>
class Map final : public CollectionBase {
 public:
  Map(Effects& effects, unsigned index, const char* name, std::size_t entries);

  const std::vector<T*>& members() const { return members_; }

  // The member under `key`, or null.
  T* find(uint64_t key) const {
    const std::size_t at = position_of_key(key);
    return at < members_.size() ? members_[at] : nullptr;
  }

  // The member under `key`, for map(key); a violation if there is none.
  T* lookup(Runtime& rt, unsigned site, uint64_t key) const {
    if (T* o = find(key)) return o;
    rt.violation(site, "%s has no member with key %llu", name(), static_cast<unsigned long long>(key));
  }

  void apply(Runtime& rt) override {
    for (const Staged<T>& r : removes_) {
      const std::size_t at = static_cast<std::size_t>(std::find(members_.begin(), members_.end(), r.object) -
                                                      members_.begin());
      members_.erase(members_.begin() + static_cast<std::ptrdiff_t>(at));
      keys_.erase(keys_.begin() + static_cast<std::ptrdiff_t>(at));
      left(*r.object);
    }
    for (const Staged<T>& a : adds_) {
      const auto key = static_cast<unsigned long long>(a.key);
      if (a.key >= entries_)
        rt.violation(a.site, "%s#%llu joins %s with key %llu, outside its %zu entries", type_name(*a.object),
                     a.object->id, name(), key, entries_);
      const std::size_t at = position_of_key(a.key);
      if (at < members_.size())
        rt.violation(a.site, "%s#%llu joins %s with key %llu, which %s#%llu holds", type_name(*a.object), a.object->id,
                     name(), key, type_name(*members_[at]), members_[at]->id);
      members_.push_back(a.object);
      keys_.push_back(a.key);
      joined(*a.object);
    }
    removes_.clear();
    adds_.clear();
  }

 private:
  friend class Effects;

  std::size_t position_of_key(uint64_t key) const {
    return static_cast<std::size_t>(std::find(keys_.begin(), keys_.end(), key) - keys_.begin());
  }

  std::vector<T*> members_;
  std::vector<uint64_t> keys_;
  std::vector<Staged<T>> adds_, removes_;
  std::size_t entries_;
};

// Stages a cycle's effects, flags every conflict between them that it can detect, and applies them at the end of
// the cycle, so that every query in the cycle reads the state before the edge.
class Effects {
 public:
  void add_collection(CollectionBase* c) { collections_.push_back(c); }

 protected:
  // Assigns field `bit`, named `field`, of o; `set_once_taken` when it is a setOnce field that already has a value.
  template <class T>
  void assign(Runtime& rt, unsigned site, unsigned long long event, T& o, unsigned bit, const char* field,
              bool set_once_taken, std::function<void()> write) {
    touch(rt, site, event, o);
    if (set_once_taken)
      rt.violation(site, "%s#%llu.%s is setOnce and was assigned in an earlier cycle", type_name(o), o.id, field);
    if (o.assigned & (uint64_t(1) << bit))
      rt.violation(site, "%s#%llu.%s is assigned twice in one cycle, also by site %u (%s)", type_name(o), o.id, field,
                   o.earlier_site, rt.site_name(o.earlier_site));
    o.assigned |= uint64_t(1) << bit;
    writes_.push_back(std::move(write));
  }

  template <class T>
  void add(Runtime& rt, unsigned site, unsigned long long event, Collection<T>& c, T& o) {
    touch(rt, site, event, o);
    if (member(o, c)) rt.violation(site, "%s#%llu is already a member of %s", type_name(o), o.id, c.name());
    if (std::find(c.adds_.begin(), c.adds_.end(), &o) != c.adds_.end())
      rt.violation(site, "%s#%llu is added to %s twice in one cycle", type_name(o), o.id, c.name());
    c.adds_.push_back(&o);
  }

  template <class T>
  void remove(Runtime& rt, unsigned site, unsigned long long event, Collection<T>& c, T& o) {
    touch(rt, site, event, o);
    if (!member(o, c)) rt.violation(site, "%s#%llu is not a member of %s", type_name(o), o.id, c.name());
    if (std::find(c.removes_.begin(), c.removes_.end(), &o) != c.removes_.end())
      rt.violation(site, "%s#%llu is removed from %s twice in one cycle", type_name(o), o.id, c.name());
    c.removes_.push_back(&o);
  }

  // o joins queue q at its tail.
  template <class T>
  void add(Runtime& rt, unsigned site, unsigned long long event, Queue<T>& q, T& o) {
    stage_join(rt, site, event, q, o, 0);
  }

  // o joins ROB r at its tail, with the hardware's key, which r checks against the one it allocates.
  template <class T>
  void add(Runtime& rt, unsigned site, unsigned long long event, Rob<T>& r, T& o, uint64_t key) {
    stage_join(rt, site, event, r, o, key);
  }

  // o joins map m under the hardware's key, which must be free once the cycle's removals apply.
  template <class T>
  void add(Runtime& rt, unsigned site, unsigned long long event, Map<T>& m, T& o, uint64_t key) {
    touch(rt, site, event, o);
    if (member(o, m)) rt.violation(site, "%s#%llu is already a member of %s", type_name(o), o.id, m.name());
    if (std::any_of(m.adds_.begin(), m.adds_.end(), [&](const Staged<T>& a) { return a.object == &o; }))
      rt.violation(site, "%s#%llu is added to %s twice in one cycle", type_name(o), o.id, m.name());
    m.adds_.push_back({&o, site, key});
  }

  template <class T>
  void remove(Runtime& rt, unsigned site, unsigned long long event, Map<T>& m, T& o) {
    touch(rt, site, event, o);
    if (!member(o, m)) rt.violation(site, "%s#%llu is not a member of %s", type_name(o), o.id, m.name());
    if (std::any_of(m.removes_.begin(), m.removes_.end(), [&](const Staged<T>& r) { return r.object == &o; }))
      rt.violation(site, "%s#%llu is removed from %s twice in one cycle", type_name(o), o.id, m.name());
    m.removes_.push_back({&o, site, 0});
  }

  // o leaves the head of queue or ROB q; the cycle's pops must take its first members.
  template <class T>
  void pop(Runtime& rt, unsigned site, unsigned long long event, Queue<T>& q, T& o) {
    stage_leave(rt, site, event, q, o, q.pops_);
  }

  // o leaves queue or ROB q through rollback, clear, or dropYoungest, which chose it.
  template <class T>
  void drop(Runtime& rt, unsigned site, unsigned long long event, Queue<T>& q, T& o) {
    stage_leave(rt, site, event, q, o, q.drops_);
  }

  template <class T>
  void destroy(Runtime& rt, unsigned site, unsigned long long event, Store<T>& store, T& o) {
    touch(rt, site, event, o);
    if (o.several)
      rt.violation(site, "%s#%llu is destroyed, but site %u (%s) also has an effect on it in this cycle",
                   type_name(o), o.id, o.earlier_site, rt.site_name(o.earlier_site));
    o.destroyed = true;
    o.destroy_site = site;
    destroys_.push_back({site, &o, type_name(o), [&store, id = o.id] { store.erase(id); }});
  }

  // Applies the cycle's effects: field writes, then membership changes, then destruction, which requires the
  // object to have left every collection.
  void apply(Runtime& rt) {
    for (auto& write : writes_) write();
    writes_.clear();
    for (CollectionBase* c : collections_) c->apply(rt);
    for (const Destroy& d : destroys_) {
      if (!d.object->member_of.empty())
        rt.violation(d.site, "%s#%llu is still in %s after its destroy", d.type, d.object->id,
                     collection(d.object->member_of.front())->name());
      d.erase();
    }
    destroys_.clear();
  }

 private:
  struct Destroy {
    unsigned site;
    Object* object;
    const char* type;
    std::function<void()> erase;
  };

  // Records that `event` has an effect on o, and flags an effect from another event once o is being destroyed.
  template <class T>
  void touch(Runtime& rt, unsigned site, unsigned long long event, T& o) {
    if (o.effect_cycle != rt.cycle()) {
      o.effect_cycle = rt.cycle();
      o.last_event = event;
      o.last_site = site;
      o.several = false;
      o.destroyed = false;
      o.assigned = 0;
      return;
    }
    if (event == o.last_event) return;
    if (o.destroyed)
      rt.violation(site, "%s#%llu has an effect here, but site %u (%s) destroys it in this cycle", type_name(o), o.id,
                   o.destroy_site, rt.site_name(o.destroy_site));
    o.several = true;
    o.earlier_site = o.last_site;
    o.last_event = event;
    o.last_site = site;
  }

  template <class T>
  void stage_join(Runtime& rt, unsigned site, unsigned long long event, Queue<T>& q, T& o, uint64_t key) {
    touch(rt, site, event, o);
    if (member(o, q)) rt.violation(site, "%s#%llu is already a member of %s", type_name(o), o.id, q.name());
    if (std::any_of(q.adds_.begin(), q.adds_.end(), [&](const Staged<T>& a) { return a.object == &o; }))
      rt.violation(site, "%s#%llu is added to %s twice in one cycle", type_name(o), o.id, q.name());
    q.adds_.push_back({&o, site, key});
  }

  template <class T>
  void stage_leave(Runtime& rt, unsigned site, unsigned long long event, Queue<T>& q, T& o,
                   std::vector<Staged<T>>& staged) {
    touch(rt, site, event, o);
    if (!member(o, q)) rt.violation(site, "%s#%llu is not a member of %s", type_name(o), o.id, q.name());
    if (q.leaving(o)) rt.violation(site, "%s#%llu is removed from %s twice in one cycle", type_name(o), o.id, q.name());
    staged.push_back({&o, site, 0});
  }

  // Whether o was in c before the clock edge.
  static bool member(const Object& o, const CollectionBase& c) {
    return std::find(o.member_of.begin(), o.member_of.end(), c.index()) != o.member_of.end();
  }

  CollectionBase* collection(unsigned index) const {
    return *std::find_if(collections_.begin(), collections_.end(), [&](CollectionBase* c) { return c->index() == index; });
  }

  std::vector<CollectionBase*> collections_;
  std::vector<std::function<void()>> writes_;
  std::vector<Destroy> destroys_;
};

template <class T>
Collection<T>::Collection(Effects& effects, unsigned index, const char* name) : CollectionBase(index, name) {
  effects.add_collection(this);
}

template <class T>
Queue<T>::Queue(Effects& effects, unsigned index, const char* name, std::size_t entries)
    : CollectionBase(index, name), entries_(entries) {
  effects.add_collection(this);
}

template <class T>
Map<T>::Map(Effects& effects, unsigned index, const char* name, std::size_t entries)
    : CollectionBase(index, name), entries_(entries) {
  effects.add_collection(this);
}

// Query helpers for generated handlers. An unset field equals no value, as a floating-point NaN does: == and the
// ordering comparisons are false when either side is unset, and != is true.
namespace query {

template <class T>
const T* value_of(const std::optional<T>& v) {
  return v ? &*v : nullptr;
}
template <class T>
const T* value_of(const T& v) {
  return &v;
}

template <class A, class B>
bool eq(const A& a, const B& b) {
  const auto* x = value_of(a);
  const auto* y = value_of(b);
  return x && y && *x == *y;
}
template <class A, class B>
bool ne(const A& a, const B& b) {
  return !eq(a, b);
}
template <class A, class B>
bool lt(const A& a, const B& b) {
  const auto* x = value_of(a);
  const auto* y = value_of(b);
  return x && y && *x < *y;
}
template <class A, class B>
bool le(const A& a, const B& b) {
  const auto* x = value_of(a);
  const auto* y = value_of(b);
  return x && y && *x <= *y;
}
template <class A, class B>
bool gt(const A& a, const B& b) {
  return lt(b, a);
}
template <class A, class B>
bool ge(const A& a, const B& b) {
  return le(b, a);
}

// The members satisfying `pred`, in the input's order.
template <class T, class P>
std::vector<T*> where(const std::vector<T*>& in, P pred) {
  std::vector<T*> out;
  for (T* m : in)
    if (pred(m)) out.push_back(m);
  return out;
}

// The members ranked by `key`, ascending; ties keep the input's order. A member whose key is unset is a violation.
template <class T, class K>
std::vector<T*> order_by(Runtime& rt, unsigned site, std::vector<T*> in, K key, const char* view, const char* key_name) {
  for (T* m : in)
    if (!value_of(key(m)))
      rt.violation(site, "%s#%llu has no %s to rank by in %s", T::kTypeName, m->id, key_name, view);
  std::stable_sort(in.begin(), in.end(), [&](T* a, T* b) { return *value_of(key(a)) < *value_of(key(b)); });
  return in;
}

// The members' names, such as ": instruction#7, instruction#9", for diagnostics.
template <class T>
std::string names(const std::vector<T*>& v) {
  std::string s;
  for (std::size_t i = 0; i < v.size() && i < 8; ++i)
    s += (i ? ", " : ": ") + std::string(T::kTypeName) + "#" + std::to_string(v[i]->id);
  if (v.size() > 8) s += ", ...";
  return s;
}

template <class T>
T* only(Runtime& rt, unsigned site, const std::vector<T*>& v, const char* view) {
  if (v.size() != 1) rt.violation(site, "only found %zu members of %s%s", v.size(), view, names(v).c_str());
  return v.front();
}
template <class T>
T* head(Runtime& rt, unsigned site, const std::vector<T*>& v, const char* view) {
  if (v.empty()) rt.violation(site, "head found no members of %s", view);
  return v.front();
}
template <class T>
T* last(Runtime& rt, unsigned site, const std::vector<T*>& v, const char* view) {
  if (v.empty()) rt.violation(site, "last found no members of %s", view);
  return v.back();
}
template <class T>
T* at(Runtime& rt, unsigned site, const std::vector<T*>& v, std::size_t k, const char* view) {
  if (k >= v.size()) rt.violation(site, "at(%zu) found only %zu members of %s%s", k, v.size(), view, names(v).c_str());
  return v[k];
}

}  // namespace query
}  // namespace eventtrace
