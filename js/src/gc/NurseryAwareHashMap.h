/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_NurseryAwareHashMap_h
#define gc_NurseryAwareHashMap_h

namespace js {

namespace detail {
// This class only handles the incremental case and does not deal with nursery
// pointers. The only users should be for NurseryAwareHashMap; it is defined
// externally because we need a GCPolicy for its use in the contained map.
template <typename T>
class UnsafeBareReadBarriered : public ReadBarrieredBase<T>
{
  public:
    UnsafeBareReadBarriered() : ReadBarrieredBase<T>(JS::GCPolicy<T>::initial()) {}
    MOZ_IMPLICIT UnsafeBareReadBarriered(const T& v) : ReadBarrieredBase<T>(v) {}
    explicit UnsafeBareReadBarriered(const UnsafeBareReadBarriered& v) : ReadBarrieredBase<T>(v) {}
    UnsafeBareReadBarriered(UnsafeBareReadBarriered&& v)
      : ReadBarrieredBase<T>(mozilla::Move(v))
    {}

    UnsafeBareReadBarriered& operator=(const UnsafeBareReadBarriered& v) {
        this->value = v.value;
        return *this;
    }

    UnsafeBareReadBarriered& operator=(const T& v) {
        this->value = v;
        return *this;
    }

    const T get() const {
        if (!InternalBarrierMethods<T>::isMarkable(this->value))
            return JS::GCPolicy<T>::initial();
        this->read();
        return this->value;
    }

    explicit operator bool() const {
        return bool(this->value);
    }

    const T unbarrieredGet() const { return this->value; }
    T* unsafeGet() { return &this->value; }
    T const* unsafeGet() const { return &this->value; }
};
} // namespace detail

// The "nursery aware" hash map is a special case of GCHashMap that is able to
// treat nursery allocated members weakly during a minor GC: e.g. it allows for
// nursery allocated objects to be collected during nursery GC where a normal
// hash table treats such edges strongly.
//
// Doing this requires some strong constraints on what can be stored in this
// table and how it can be accessed. At the moment, this table assumes that
// all values contain a strong reference to the key. It also requires the
// policy to contain an |isTenured| and |needsSweep| members, which is fairly
// non-standard. This limits its usefulness to the CrossCompartmentMap at the
// moment, but might serve as a useful base for other tables in future.
template <typename Key,
          typename Value,
          typename HashPolicy = DefaultHasher<Key>,
          typename AllocPolicy = TempAllocPolicy>
class NurseryAwareHashMap
{
	using BarrieredValue = detail::UnsafeBareReadBarriered<Value>;
	using MapType = GCRekeyableHashMap<Key, BarrieredValue, HashPolicy, AllocPolicy>;
	MapType map;
	
  public:
    using Lookup = typename MapType::Lookup;
    using Ptr = typename MapType::Ptr;
    using Range = typename MapType::Range;
	
    explicit NurseryAwareHashMap(AllocPolicy a = AllocPolicy()) : map(a) {}

    MOZ_MUST_USE bool init(uint32_t len = 16) { return map.init(len); }

    bool empty() const { return map.empty(); }
    Ptr lookup(const Lookup& l) const { return map.lookup(l); }
    void remove(Ptr p) { map.remove(p); }
    Range all() const { return map.all(); }
    struct Enum : public MapType::Enum {
        explicit Enum(NurseryAwareHashMap& namap) : MapType::Enum(namap.map) {}
    };
    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return map.sizeOfExcludingThis(mallocSizeOf);
    }
    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return map.sizeOfIncludingThis(mallocSizeOf);
    }

    MOZ_MUST_USE bool put(const Key& k, const Value& v) {
        auto p = map.lookupForAdd(k);
        if (p) {
            p->value() = v;
            return true;
        }
        return map.add(p, k, v);
    }

    void sweepAfterMinorGC(JSTracer* trc) {
    }

    void sweep() {
		map.sweep();
    }
};

} // namespace js

namespace JS {
template <typename T>
struct GCPolicy<js::detail::UnsafeBareReadBarriered<T>>
{
    static void trace(JSTracer* trc, js::detail::UnsafeBareReadBarriered<T>* thingp,
                      const char* name)
    {
    }
    static bool needsSweep(js::detail::UnsafeBareReadBarriered<T>* thingp) {
        return false;
    }
};
} // namespace JS

#endif // gc_NurseryAwareHashMap_h
