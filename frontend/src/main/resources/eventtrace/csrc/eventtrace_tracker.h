// EventTrace object tracking: what a generated sites::Tracker is built from. Objects live in stores, collections
// keep their members in join order, queries scan them, and effects are staged during a cycle, checked against each
// other, and applied when it ends.
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

// What applying a cycle's membership changes needs from a collection of any type.
class CollectionBase {
 public:
  CollectionBase(unsigned index, const char* name) : index_(index), name_(name) {}
  virtual ~CollectionBase() = default;
  unsigned index() const { return index_; }
  const char* name() const { return name_; }

  // Applies the cycle's staged removals, then its additions, which join in the order they were staged.
  virtual void apply() = 0;

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

  void apply() override {
    for (T* o : removes_) {
      members_.erase(std::find(members_.begin(), members_.end(), o));
      o->member_of.erase(std::find(o->member_of.begin(), o->member_of.end(), index()));
    }
    for (T* o : adds_) {
      members_.push_back(o);
      o->member_of.push_back(index());
    }
    removes_.clear();
    adds_.clear();
  }

 private:
  friend class Effects;
  std::vector<T*> members_, adds_, removes_;
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
      rt.violation(site, "%s#%llu.%s is setOnce and was assigned in an earlier cycle", T::kTypeName, o.id, field);
    if (o.assigned & (uint64_t(1) << bit))
      rt.violation(site, "%s#%llu.%s is assigned twice in one cycle, also by site %u (%s)", T::kTypeName, o.id, field,
                   o.earlier_site, rt.site_name(o.earlier_site));
    o.assigned |= uint64_t(1) << bit;
    writes_.push_back(std::move(write));
  }

  template <class T>
  void add(Runtime& rt, unsigned site, unsigned long long event, Collection<T>& c, T& o) {
    touch(rt, site, event, o);
    if (member(o, c)) rt.violation(site, "%s#%llu is already a member of %s", T::kTypeName, o.id, c.name());
    if (std::find(c.adds_.begin(), c.adds_.end(), &o) != c.adds_.end())
      rt.violation(site, "%s#%llu is added to %s twice in one cycle", T::kTypeName, o.id, c.name());
    c.adds_.push_back(&o);
  }

  template <class T>
  void remove(Runtime& rt, unsigned site, unsigned long long event, Collection<T>& c, T& o) {
    touch(rt, site, event, o);
    if (!member(o, c)) rt.violation(site, "%s#%llu is not a member of %s", T::kTypeName, o.id, c.name());
    if (std::find(c.removes_.begin(), c.removes_.end(), &o) != c.removes_.end())
      rt.violation(site, "%s#%llu is removed from %s twice in one cycle", T::kTypeName, o.id, c.name());
    c.removes_.push_back(&o);
  }

  template <class T>
  void destroy(Runtime& rt, unsigned site, unsigned long long event, Store<T>& store, T& o) {
    touch(rt, site, event, o);
    if (o.several)
      rt.violation(site, "%s#%llu is destroyed, but site %u (%s) also has an effect on it in this cycle",
                   T::kTypeName, o.id, o.earlier_site, rt.site_name(o.earlier_site));
    o.destroyed = true;
    o.destroy_site = site;
    destroys_.push_back({site, &o, T::kTypeName, [&store, id = o.id] { store.erase(id); }});
  }

  // Applies the cycle's effects: field writes, then membership changes, then destruction, which requires the
  // object to have left every collection.
  void apply(Runtime& rt) {
    for (auto& write : writes_) write();
    writes_.clear();
    for (CollectionBase* c : collections_) c->apply();
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
      rt.violation(site, "%s#%llu has an effect here, but site %u (%s) destroys it in this cycle", T::kTypeName, o.id,
                   o.destroy_site, rt.site_name(o.destroy_site));
    o.several = true;
    o.earlier_site = o.last_site;
    o.last_event = event;
    o.last_site = site;
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
