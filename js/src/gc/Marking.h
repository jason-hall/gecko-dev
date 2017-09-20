/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Marking_h
#define gc_Marking_h

#include "mozilla/HashFunctions.h"
#include "mozilla/Move.h"

#include "jsfriendapi.h"

#include "ds/OrderedHashTable.h"
#include "gc/Heap.h"
#include "gc/Tracer.h"
#include "js/GCAPI.h"
#include "js/HeapAPI.h"
#include "js/SliceBudget.h"
#include "js/TracingAPI.h"
#include "threading/ProtectedData.h"
#include "vm/TaggedProto.h"

class JSLinearString;
class JSRope;
namespace js {
class BaseShape;
class GCMarker;
class LazyScript;
class NativeObject;
class ObjectGroup;
class WeakMapBase;
namespace jit {
class JitCode;
} // namespace jit

static const size_t NON_INCREMENTAL_MARK_STACK_BASE_CAPACITY = 4096;
static const size_t INCREMENTAL_MARK_STACK_BASE_CAPACITY = 32768;

namespace gc {

struct WeakKeyTableHashPolicy {
    typedef JS::GCCellPtr Lookup;
    static HashNumber hash(const Lookup& v, const mozilla::HashCodeScrambler&) {
        return mozilla::HashGeneric(v.asCell());
    }
    static bool match(const JS::GCCellPtr& k, const Lookup& l) { return k == l; }
    static bool isEmpty(const JS::GCCellPtr& v) { return !v; }
    static void makeEmpty(JS::GCCellPtr* vp) { *vp = nullptr; }
};

struct WeakMarkable {
    WeakMapBase* weakmap;
    JS::GCCellPtr key;

    WeakMarkable(WeakMapBase* weakmapArg, JS::GCCellPtr keyArg)
      : weakmap(weakmapArg), key(keyArg) {}
};

using WeakEntryVector = Vector<WeakMarkable, 2, js::SystemAllocPolicy>;

using WeakKeyTable = OrderedHashMap<JS::GCCellPtr,
                                    WeakEntryVector,
                                    WeakKeyTableHashPolicy,
                                    js::SystemAllocPolicy>;

} /* namespace gc */

class GCMarker : public JSTracer
{
  public:
    explicit GCMarker(JSRuntime* rt) : JSTracer(rt, JSTracer::TracerKindTag::Marking, ExpandWeakMaps) {}
    MOZ_MUST_USE bool init(JSGCMode gcMode) { return true; }

    static GCMarker* fromTracer(JSTracer* trc) {
        MOZ_ASSERT(trc->isMarkingTracer());
        return static_cast<GCMarker*>(trc);
    }
};

#ifdef DEBUG
// Return true if this trace is happening on behalf of gray buffering during
// the marking phase of incremental GC.
bool
IsBufferGrayRootsTracer(JSTracer* trc);

bool
IsUnmarkGrayTracer(JSTracer* trc);
#endif

namespace gc {

/*** Special Cases ***/

/*** Liveness ***/

bool
IsMarkedCell(const TenuredCell* const thingp);

// Report whether a thing has been marked.  Things which are in zones that are
// not currently being collected or are owned by another runtime are always
// reported as being marked.
template <typename T>
bool
IsMarkedUnbarriered(JSRuntime* rt, T* thingp);

// Report whether a thing has been marked.  Things which are in zones that are
// not currently being collected or are owned by another runtime are always
// reported as being marked.
template <typename T>
bool
IsMarked(JSRuntime* rt, WriteBarrieredBase<T>* thingp);

template <typename T>
bool
IsAboutToBeFinalizedUnbarriered(T* thingp);

template <typename T>
bool
IsAboutToBeFinalized(WriteBarrieredBase<T>* thingp);

template <typename T>
bool
IsAboutToBeFinalized(ReadBarrieredBase<T>* thingp);

bool
IsAboutToBeFinalizedDuringSweep(TenuredCell& tenured);

inline Cell*
ToMarkable(const Value& v)
{
    if (v.isGCThing())
        return (Cell*)v.toGCThing();
    return nullptr;
}

inline Cell*
ToMarkable(Cell* cell)
{
    return cell;
}

// Wrap a GC thing pointer into a new Value or jsid. The type system enforces
// that the thing pointer is a wrappable type.
template <typename S, typename T>
struct RewrapTaggedPointer{};
#define DECLARE_REWRAP(S, T, method, prefix) \
    template <> struct RewrapTaggedPointer<S, T> { \
        static S wrap(T* thing) { return method ( prefix thing ); } \
    }
DECLARE_REWRAP(JS::Value, JSObject, JS::ObjectOrNullValue, );
DECLARE_REWRAP(JS::Value, JSString, JS::StringValue, );
DECLARE_REWRAP(JS::Value, JS::Symbol, JS::SymbolValue, );
DECLARE_REWRAP(jsid, JSString, NON_INTEGER_ATOM_TO_JSID, (JSAtom*));
DECLARE_REWRAP(jsid, JS::Symbol, SYMBOL_TO_JSID, );
DECLARE_REWRAP(js::TaggedProto, JSObject, js::TaggedProto, );
#undef DECLARE_REWRAP

template <typename T>
struct IsPrivateGCThingInValue
  : public mozilla::EnableIf<mozilla::IsBaseOf<Cell, T>::value &&
                             !mozilla::IsBaseOf<JSObject, T>::value &&
                             !mozilla::IsBaseOf<JSString, T>::value &&
                             !mozilla::IsBaseOf<JS::Symbol, T>::value, T>
{
    static_assert(!mozilla::IsSame<Cell, T>::value && !mozilla::IsSame<TenuredCell, T>::value,
                  "T must not be Cell or TenuredCell");
};

template <typename T>
struct RewrapTaggedPointer<Value, T>
{
    static Value wrap(typename IsPrivateGCThingInValue<T>::Type* thing) {
        return JS::PrivateGCThingValue(thing);
    }
};

} /* namespace gc */

// The return value indicates if anything was unmarked.
bool
UnmarkGrayShapeRecursively(Shape* shape);

template<typename T>
void
CheckTracedThing(JSTracer* trc, T* thing);

template<typename T>
void
CheckTracedThing(JSTracer* trc, T thing);

} /* namespace js */

namespace JS {
class Symbol;
}

// Exported for Tracer.cpp
inline bool ThingIsPermanentAtomOrWellKnownSymbol(js::gc::Cell* thing) { return false; }
bool ThingIsPermanentAtomOrWellKnownSymbol(JSString*);
bool ThingIsPermanentAtomOrWellKnownSymbol(JSFlatString*);
bool ThingIsPermanentAtomOrWellKnownSymbol(JSLinearString*);
bool ThingIsPermanentAtomOrWellKnownSymbol(JSAtom*);
bool ThingIsPermanentAtomOrWellKnownSymbol(js::PropertyName*);
bool ThingIsPermanentAtomOrWellKnownSymbol(JS::Symbol*);

#endif /* gc_Marking_h */
