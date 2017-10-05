#if!defined(OMRGLUE_HPP_)
#define OMRGLUE_HPP_

#include "CollectorLanguageInterfaceImpl.hpp"
#include "MarkingScheme.hpp"

#include "js/TracingAPI.h"

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
#include "vm/TaggedProto.h"

// OMR
#include "omr/gc/base/MarkingScheme.hpp"
#include "omr/gc/base/EnvironmentBase.hpp"

class JSLinearString;
class JSRope;
namespace js {
class BaseShape;
class GCMarker;
class LazyScript;
class NativeObject;
class ObjectGroup;
class WeakMapBase;
namespace gc {
class Arena;
} // namespace gc
namespace jit {
class JitCode;
} // namespace jit
} // namespace js

namespace omrjs {

using namespace js;
using namespace JS;

class OMRGCMarker : public JSTracer
{
public:
    explicit OMRGCMarker(JSRuntime* rt, MM_EnvironmentBase* env, MM_MarkingScheme* ms);

    // Mark the given GC thing and traverse its children at some point.
    template <typename T> inline void traverse(T *thing);
    template <typename T> inline void traverse(T **thing);

    // Calls traverse on target after making additional assertions.
    template <typename S, typename T> void traverseEdge(S source, T* target);
    template <typename S, typename T> void traverseEdge(S source, const T& target);

    // Notes a weak graph edge for later sweeping.
    template <typename T> void noteWeakEdge(T* edge);

    static OMRGCMarker* fromTracer(JSTracer* trc) {
        return static_cast<OMRGCMarker*>(trc);
    }

private:
    MM_EnvironmentBase* _env;
    MM_MarkingScheme* _markingScheme;

    // We may not have concrete types yet, so this has to be outside the header.
    template <typename T>
    void dispatchToTraceChildren(T* thing);

    // Mark the given GC thing, but do not trace its children. Return true
    // if the thing became marked.
    template <typename T>
    bool mark(T* thing) {
		_markingScheme->markObject(_env, (omrobjectptr_t)thing, false);
		return true;
	}
};

template <typename T> inline void OMRGCMarker::traverse(T *thing) { assert(0); }
template <typename T> inline void OMRGCMarker::traverse(T **thing) { traverse(*thing); }
template <> inline void OMRGCMarker::traverse(BaseShape* thing) { mark(thing); }
template <> inline void OMRGCMarker::traverse(JS::Symbol* thing) { mark(thing); }
template <> inline void OMRGCMarker::traverse(JSString* thing) { mark(thing); }
template <> inline void OMRGCMarker::traverse(LazyScript* thing) { mark(thing); }
template <> inline void OMRGCMarker::traverse(Shape* thing) { mark(thing); }
template <> inline void OMRGCMarker::traverse(js::Scope* thing) { mark(thing); }
template <> inline void OMRGCMarker::traverse(JSObject* thing) { mark(thing); }
template <> inline void OMRGCMarker::traverse(ObjectGroup* thing) { mark(thing); }
template <> inline void OMRGCMarker::traverse(jit::JitCode* thing) { mark(thing); }
template <> inline void OMRGCMarker::traverse(JSScript* thing) { mark(thing); }
template <> inline void OMRGCMarker::traverse(RegExpShared* thing) { mark(thing); }

template <> inline void OMRGCMarker::traverse(js::TaggedProto* thing) {
	if (thing->isObject()) {
		traverse(thing->toObject());
	}
}

template <> inline void OMRGCMarker::traverse(jsid* thing) {
	if (JSID_IS_GCTHING(*thing)) {
		if (JSID_IS_STRING(*thing)) {
			traverse(JSID_TO_STRING(*thing));
		} else if (JSID_IS_SYMBOL(*thing)) {
			traverse(JSID_TO_SYMBOL(*thing));
		}
	}
}

template <> inline void OMRGCMarker::traverse(JS::Value* thing) {
	const Value& v = *thing;
	if (v.isString()) {
		JSString* string = v.toString();
		traverse(string);
	} else if (v.isObject()) {
		JSObject* obj = &v.toObject();
		traverse(obj);
	} else if (v.isSymbol()) {
		JS::Symbol* sym = v.toSymbol();
		traverse(sym);
	} else if (v.isPrivateGCThing()) {
		js::gc::Cell *cell = v.toGCCellPtr().asCell();
		_markingScheme->markObject(_env, (omrobjectptr_t)cell, false);
	}
}

} // namespace omrjs

#endif // OMRGLUE_HPP_
