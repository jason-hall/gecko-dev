/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Marking.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/IntegerRange.h"
#include "mozilla/ReentrancyGuard.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/TypeTraits.h"

#include "jsgc.h"
#include "jsprf.h"

#include "builtin/ModuleObject.h"
#include "gc/GCInternals.h"
#include "gc/Policy.h"
#include "jit/IonCode.h"
#include "js/SliceBudget.h"
#include "vm/ArgumentsObject.h"
#include "vm/ArrayObject.h"
#include "vm/Debugger.h"
#include "vm/EnvironmentObject.h"
#include "vm/RegExpObject.h"
#include "vm/RegExpShared.h"
#include "vm/Scope.h"
#include "vm/Shape.h"
#include "vm/Symbol.h"
#include "vm/TypedArrayObject.h"
#include "vm/UnboxedObject.h"
#include "wasm/WasmJS.h"

#include "jscompartmentinlines.h"
#include "jsgcinlines.h"
#include "jsobjinlines.h"

#include "gc/Nursery-inl.h"
#include "vm/String-inl.h"
#include "vm/UnboxedObject-inl.h"

#include "../glue/omrglue.hpp"
#include "../omr/gc/base/standard/ParallelGlobalGC.hpp"

using namespace js;
using namespace js::gc;

using JS::MapTypeToTraceKind;

using mozilla::ArrayLength;
using mozilla::DebugOnly;
using mozilla::IntegerRange;
using mozilla::IsBaseOf;
using mozilla::IsSame;
using mozilla::PodCopy;

// Tracing Overview
// ================
//
// Tracing, in this context, refers to an abstract visitation of some or all of
// the GC-controlled heap. The effect of tracing an edge of the graph depends
// on the subclass of the JSTracer on whose behalf we are tracing.
//
// Marking
// -------
//
// The primary JSTracer is the GCMarker. The marking tracer causes the target
// of each traversed edge to be marked black and the target edge's children to
// be marked either gray (in the gc algorithm sense) or immediately black.
//
// Callback
// --------
//
// The secondary JSTracer is the CallbackTracer. This simply invokes a callback
// on each edge in a child.
//
// The following is a rough outline of the general struture of the tracing
// internals.
//
//                                                                                              //
//   .---------.    .---------.    .--------------------------.       .----------.              //
//   |TraceEdge|    |TraceRoot|    |TraceManuallyBarrieredEdge|  ...  |TraceRange|   ... etc.   //
//   '---------'    '---------'    '--------------------------'       '----------'              //
//        \              \                        /                        /                    //
//         \              \  .----------------.  /                        /                     //
//          o------------->o-|DispatchToTracer|-o<-----------------------o                      //
//                           '----------------'                                                 //
//                              /          \                                                    //
//                             /            \                                                   //
//                       .---------.   .----------.         .-----------------.                 //
//                       |DoMarking|   |DoCallback|-------> |<JSTraceCallback>|----------->     //
//                       '---------'   '----------'         '-----------------'                 //
//                            |                                                                 //
//                            |                                                                 //
//                        .--------.                                                            //
//      o---------------->|traverse| .                                                          //
//     /_\                '--------'   ' .                                                      //
//      |                     .     .      ' .                                                  //
//      |                     .       .        ' .                                              //
//      |                     .         .          ' .                                          //
//      |             .-----------.    .-----------.   ' .     .--------------------.           //
//      |             |markAndScan|    |markAndPush|       ' - |markAndTraceChildren|---->      //
//      |             '-----------'    '-----------'           '--------------------'           //
//      |                   |                  \                                                //
//      |                   |                   \                                               //
//      |       .----------------------.     .----------------.                                 //
//      |       |T::eagerlyMarkChildren|     |pushMarkStackTop|<===Oo                           //
//      |       '----------------------'     '----------------'    ||                           //
//      |                  |                         ||            ||                           //
//      |                  |                         ||            ||                           //
//      |                  |                         ||            ||                           //
//      o<-----------------o<========================OO============Oo                           //
//                                                                                              //
//                                                                                              //
//   Legend:                                                                                    //
//     ------  Direct calls                                                                     //
//     . . .   Static dispatch                                                                  //
//     ======  Dispatch through a manual stack.                                                 //
//                                                                                              //


/*** Tracing Invariants **************************************************************************/

#if defined(DEBUG)
template<typename T>
static inline bool
IsThingPoisoned(T* thing)
{
    const uint8_t poisonBytes[] = {
        JS_FRESH_NURSERY_PATTERN,
        JS_SWEPT_NURSERY_PATTERN,
        JS_ALLOCATED_NURSERY_PATTERN,
        JS_FRESH_TENURED_PATTERN,
        JS_MOVED_TENURED_PATTERN,
        JS_SWEPT_TENURED_PATTERN,
        JS_ALLOCATED_TENURED_PATTERN,
        JS_SWEPT_CODE_PATTERN
    };
    const int numPoisonBytes = sizeof(poisonBytes) / sizeof(poisonBytes[0]);
    uint32_t* p = reinterpret_cast<uint32_t*>(reinterpret_cast<FreeSpan*>(thing) + 1);
    // Note: all free patterns are odd to make the common, not-poisoned case a single test.
    if ((*p & 1) == 0)
        return false;
    for (int i = 0; i < numPoisonBytes; ++i) {
        const uint8_t pb = poisonBytes[i];
        const uint32_t pw = pb | (pb << 8) | (pb << 16) | (pb << 24);
        if (*p == pw)
            return true;
    }
    return false;
}
#endif

template <typename T>
static inline bool
IsOwnedByOtherRuntime(JSRuntime* rt, T thing)
{
    bool other = thing->runtimeFromAnyThread() != rt;
    return other;
}

template<typename T>
void
js::CheckTracedThing(JSTracer* trc, T* thing)
{
#ifdef DEBUG
    MOZ_ASSERT(trc);
    MOZ_ASSERT(thing);

    if (!trc->checkEdges())
        return;

    if (IsForwarded(thing))
        thing = Forwarded(thing);

    /* This function uses data that's not available in the nursery. */
    /*if (IsInsideNursery(thing))
        return;*/

    /*
     * Permanent atoms are not associated with this runtime, but will be
     * ignored during marking.
     */
    if (IsOwnedByOtherRuntime(trc->runtime(), thing))
        return;

    Zone* zone = thing->zoneFromAnyThread();

    MOZ_ASSERT(zone->runtimeFromAnyThread() == trc->runtime());

    MOZ_ASSERT(thing->isAligned());
    MOZ_ASSERT(MapTypeToTraceKind<typename mozilla::RemovePointer<T>::Type>::kind ==
               thing->getTraceKind());
#endif
}

template <typename S>
struct CheckTracedFunctor : public VoidDefaultAdaptor<S> {
    template <typename T> void operator()(T* t, JSTracer* trc) { CheckTracedThing(trc, t); }
};

template<typename T>
void
js::CheckTracedThing(JSTracer* trc, T thing)
{
    DispatchTyped(CheckTracedFunctor<T>(), thing, trc);
}

namespace js {
#define IMPL_CHECK_TRACED_THING(_, type, __) \
    template void CheckTracedThing<type>(JSTracer*, type*);
JS_FOR_EACH_TRACEKIND(IMPL_CHECK_TRACED_THING);
#undef IMPL_CHECK_TRACED_THING
} // namespace js


/*** Tracing Interface ***************************************************************************/

// The second parameter to BaseGCType is derived automatically based on T. The
// relation here is that for any T, the TraceKind will automatically,
// statically select the correct Cell layout for marking. Below, we instantiate
// each override with a declaration of the most derived layout type.
//
// The use of TraceKind::Null for the case where the type is not matched
// generates a compile error as no template instantiated for that kind.
//
// Usage:
//   BaseGCType<T>::type
//
// Examples:
//   BaseGCType<JSFunction>::type => JSObject
//   BaseGCType<UnownedBaseShape>::type => BaseShape
//   etc.
template <typename T, JS::TraceKind =
#define EXPAND_MATCH_TYPE(name, type, _) \
          IsBaseOf<type, T>::value ? JS::TraceKind::name :
JS_FOR_EACH_TRACEKIND(EXPAND_MATCH_TYPE)
#undef EXPAND_MATCH_TYPE
          JS::TraceKind::Null>

struct BaseGCType;
#define IMPL_BASE_GC_TYPE(name, type_, _) \
    template <typename T> struct BaseGCType<T, JS::TraceKind:: name> { typedef type_ type; };
JS_FOR_EACH_TRACEKIND(IMPL_BASE_GC_TYPE);
#undef IMPL_BASE_GC_TYPE

// Our barrier templates are parameterized on the pointer types so that we can
// share the definitions with Value and jsid. Thus, we need to strip the
// pointer before sending the type to BaseGCType and re-add it on the other
// side. As such:
template <typename T> struct PtrBaseGCType { typedef T type; };
template <typename T> struct PtrBaseGCType<T*> { typedef typename BaseGCType<T>::type* type; };

template <typename T>
typename PtrBaseGCType<T>::type*
ConvertToBase(T* thingp)
{
    return reinterpret_cast<typename PtrBaseGCType<T>::type*>(thingp);
}

template <typename T> void DispatchToTracer(JSTracer* trc, T* thingp, const char* name);
template <typename T> T DoCallback(JS::CallbackTracer* trc, T* thingp, const char* name);

template <typename T>
void
js::TraceEdge(JSTracer* trc, WriteBarrieredBase<T>* thingp, const char* name)
{
    DispatchToTracer(trc, ConvertToBase(thingp->unsafeUnbarrieredForTracing()), name);
}

template <typename T>
void
js::TraceEdge(JSTracer* trc, ReadBarriered<T>* thingp, const char* name)
{
    DispatchToTracer(trc, ConvertToBase(thingp->unsafeGet()), name);
}

template <typename T>
void
js::TraceNullableEdge(JSTracer* trc, WriteBarrieredBase<T>* thingp, const char* name)
{
    if (InternalBarrierMethods<T>::isMarkable(thingp->get()))
        DispatchToTracer(trc, ConvertToBase(thingp->unsafeUnbarrieredForTracing()), name);
}

template <typename T>
void
js::TraceNullableEdge(JSTracer* trc, ReadBarriered<T>* thingp, const char* name)
{
    if (InternalBarrierMethods<T>::isMarkable(thingp->unbarrieredGet()))
        DispatchToTracer(trc, ConvertToBase(thingp->unsafeGet()), name);
}

template <typename T>
JS_PUBLIC_API(void)
js::gc::TraceExternalEdge(JSTracer* trc, T* thingp, const char* name)
{
    MOZ_ASSERT(InternalBarrierMethods<T>::isMarkable(*thingp));
    DispatchToTracer(trc, ConvertToBase(thingp), name);
}

template <typename T>
void
js::TraceManuallyBarrieredEdge(JSTracer* trc, T* thingp, const char* name)
{
    DispatchToTracer(trc, ConvertToBase(thingp), name);
}

template <typename T>
JS_PUBLIC_API(void)
js::UnsafeTraceManuallyBarrieredEdge(JSTracer* trc, T* thingp, const char* name)
{
    DispatchToTracer(trc, ConvertToBase(thingp), name);
}

template <typename T>
void
js::TraceWeakEdge(JSTracer* trc, WeakRef<T>* thingp, const char* name)
{
    if (!trc->isMarkingTracer()) {
        // Non-marking tracers can select whether or not they see weak edges.
        if (trc->traceWeakEdges())
            DispatchToTracer(trc, ConvertToBase(thingp->unsafeUnbarrieredForTracing()), name);
        return;
    }
}

template <typename T>
void
js::TraceRoot(JSTracer* trc, T* thingp, const char* name)
{
    DispatchToTracer(trc, ConvertToBase(thingp), name);
}

template <typename T>
void
js::TraceRoot(JSTracer* trc, ReadBarriered<T>* thingp, const char* name)
{
    TraceRoot(trc, thingp->unsafeGet(), name);
}

template <typename T>
void
js::TraceNullableRoot(JSTracer* trc, T* thingp, const char* name)
{
    if (InternalBarrierMethods<T>::isMarkable(*thingp))
        DispatchToTracer(trc, ConvertToBase(thingp), name);
}

template <typename T>
void
js::TraceNullableRoot(JSTracer* trc, ReadBarriered<T>* thingp, const char* name)
{
    TraceNullableRoot(trc, thingp->unsafeGet(), name);
}

template <typename T>
JS_PUBLIC_API(void)
JS::UnsafeTraceRoot(JSTracer* trc, T* thingp, const char* name)
{
    MOZ_ASSERT(thingp);
    js::TraceNullableRoot(trc, thingp, name);
}

template <typename T>
void
js::TraceRange(JSTracer* trc, size_t len, WriteBarrieredBase<T>* vec, const char* name)
{
    JS::AutoTracingIndex index(trc);
    for (auto i : IntegerRange(len)) {
        if (InternalBarrierMethods<T>::isMarkable(vec[i].get()))
            DispatchToTracer(trc, ConvertToBase(vec[i].unsafeUnbarrieredForTracing()), name);
        ++index;
    }
}

template <typename T>
void
js::TraceRootRange(JSTracer* trc, size_t len, T* vec, const char* name)
{
    JS::AutoTracingIndex index(trc);
    for (auto i : IntegerRange(len)) {
        if (InternalBarrierMethods<T>::isMarkable(vec[i]))
            DispatchToTracer(trc, ConvertToBase(&vec[i]), name);
        ++index;
    }
}

// Instantiate a copy of the Tracing templates for each derived type.
#define INSTANTIATE_ALL_VALID_TRACE_FUNCTIONS(type) \
    template void js::TraceEdge<type>(JSTracer*, WriteBarrieredBase<type>*, const char*); \
    template void js::TraceEdge<type>(JSTracer*, ReadBarriered<type>*, const char*); \
    template void js::TraceNullableEdge<type>(JSTracer*, WriteBarrieredBase<type>*, const char*); \
    template void js::TraceNullableEdge<type>(JSTracer*, ReadBarriered<type>*, const char*); \
    template void js::TraceManuallyBarrieredEdge<type>(JSTracer*, type*, const char*); \
    template void js::TraceWeakEdge<type>(JSTracer*, WeakRef<type>*, const char*); \
    template void js::TraceRoot<type>(JSTracer*, type*, const char*); \
    template void js::TraceRoot<type>(JSTracer*, ReadBarriered<type>*, const char*); \
    template void js::TraceNullableRoot<type>(JSTracer*, type*, const char*); \
    template void js::TraceNullableRoot<type>(JSTracer*, ReadBarriered<type>*, const char*); \
    template void js::TraceRange<type>(JSTracer*, size_t, WriteBarrieredBase<type>*, const char*); \
    template void js::TraceRootRange<type>(JSTracer*, size_t, type*, const char*);
FOR_EACH_GC_POINTER_TYPE(INSTANTIATE_ALL_VALID_TRACE_FUNCTIONS)
#undef INSTANTIATE_ALL_VALID_TRACE_FUNCTIONS

#define INSTANTIATE_PUBLIC_TRACE_FUNCTIONS(type) \
    template JS_PUBLIC_API(void) JS::TraceEdge<type>(JSTracer*, JS::Heap<type>*, const char*); \
    template JS_PUBLIC_API(void) JS::UnsafeTraceRoot<type>(JSTracer*, type*, const char*); \
    template JS_PUBLIC_API(void) js::UnsafeTraceManuallyBarrieredEdge<type>(JSTracer*, type*, \
                                                                            const char*);
FOR_EACH_PUBLIC_GC_POINTER_TYPE(INSTANTIATE_PUBLIC_TRACE_FUNCTIONS)
FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(INSTANTIATE_PUBLIC_TRACE_FUNCTIONS)
#undef INSTANTIATE_PUBLIC_TRACE_FUNCTIONS

template <typename T>
void
js::TraceManuallyBarrieredCrossCompartmentEdge(JSTracer* trc, JSObject* src, T* dst,
                                               const char* name)
{
    DispatchToTracer(trc, dst, name);
}
template void js::TraceManuallyBarrieredCrossCompartmentEdge<JSObject*>(JSTracer*, JSObject*,
                                                                        JSObject**, const char*);
template void js::TraceManuallyBarrieredCrossCompartmentEdge<JSScript*>(JSTracer*, JSObject*,
                                                                        JSScript**, const char*);

template <typename T>
void
js::TraceCrossCompartmentEdge(JSTracer* trc, JSObject* src, WriteBarrieredBase<T>* dst,
                              const char* name)
{
    DispatchToTracer(trc, dst->unsafeUnbarrieredForTracing(), name);
}
template void js::TraceCrossCompartmentEdge<Value>(JSTracer*, JSObject*,
                                                   WriteBarrieredBase<Value>*, const char*);

template <typename T>
void
js::TraceProcessGlobalRoot(JSTracer* trc, T* thing, const char* name)
{
    // We have to mark permanent atoms and well-known symbols through a special
    // method because the default DoMarking implementation automatically skips
    // them. Fortunately, atoms (permanent and non) cannot refer to other GC
    // things so they do not need to go through the mark stack and may simply
    // be marked directly.  Moreover, well-known symbols can refer only to
    // permanent atoms, so likewise require no subsquent marking.
    // OMRTODO: Delete original marking tracer
    CheckTracedThing(trc, *ConvertToBase(&thing));
    if (trc->isOmrMarkingTracer())
		return static_cast<omrjs::OMRGCMarker*>(trc)->traverse(ConvertToBase(&thing));
    else if(trc->isCallbackTracer())
		DoCallback(trc->asCallbackTracer(), ConvertToBase(&thing), name);
}
template void js::TraceProcessGlobalRoot<JSAtom>(JSTracer*, JSAtom*, const char*);
template void js::TraceProcessGlobalRoot<JS::Symbol>(JSTracer*, JS::Symbol*, const char*);

// A typed functor adaptor for TraceRoot.
struct TraceRootFunctor {
    template <typename T>
    void operator()(JSTracer* trc, Cell** thingp, const char* name) {
        TraceRoot(trc, reinterpret_cast<T**>(thingp), name);
    }
};

void
js::TraceGenericPointerRoot(JSTracer* trc, Cell** thingp, const char* name)
{
    MOZ_ASSERT(thingp);
    if (!*thingp)
        return;
    TraceRootFunctor f;
    DispatchTraceKindTyped(f, (*thingp)->getTraceKind(), trc, thingp, name);
}

// A typed functor adaptor for TraceManuallyBarrieredEdge.
struct TraceManuallyBarrieredEdgeFunctor {
    template <typename T>
    void operator()(JSTracer* trc, Cell** thingp, const char* name) {
        TraceManuallyBarrieredEdge(trc, reinterpret_cast<T**>(thingp), name);
    }
};

void
js::TraceManuallyBarrieredGenericPointerEdge(JSTracer* trc, Cell** thingp, const char* name)
{
    MOZ_ASSERT(thingp);
    if (!*thingp)
        return;
    TraceManuallyBarrieredEdgeFunctor f;
    DispatchTraceKindTyped(f, (*thingp)->getTraceKind(), trc, thingp, name);
}

// This method is responsible for dynamic dispatch to the real tracer
// implementation. Consider replacing this choke point with virtual dispatch:
// a sufficiently smart C++ compiler may be able to devirtualize some paths.
template <typename T>
void
DispatchToTracer(JSTracer* trc, T* thingp, const char* name)
{
#define IS_SAME_TYPE_OR(name, type, _) mozilla::IsSame<type*, T>::value ||
    static_assert(
            JS_FOR_EACH_TRACEKIND(IS_SAME_TYPE_OR)
            mozilla::IsSame<T, JS::Value>::value ||
            mozilla::IsSame<T, jsid>::value ||
            mozilla::IsSame<T, TaggedProto>::value,
            "Only the base cell layout types are allowed into marking/tracing internals");
#undef IS_SAME_TYPE_OR
    if (trc->isOmrMarkingTracer())
        return static_cast<omrjs::OMRGCMarker*>(trc)->traverse(thingp);
    else if (trc->isCallbackTracer())
        DoCallback(trc->asCallbackTracer(), thingp, name);
}


/*** GC Marking Interface *************************************************************************/

namespace js {

typedef bool HasNoImplicitEdgesType;

template <typename T>
struct ImplicitEdgeHolderType {
    typedef HasNoImplicitEdgesType Type;
};

// For now, we only handle JSObject* and JSScript* keys, but the linear time
// algorithm can be easily extended by adding in more types here, then making
// GCMarker::traverse<T> call markPotentialEphemeronKey.
template <>
struct ImplicitEdgeHolderType<JSObject*> {
    typedef JSObject* Type;
};

template <>
struct ImplicitEdgeHolderType<JSScript*> {
    typedef JSScript* Type;
};

} // namespace js

template <typename T>
static inline bool
MustSkipMarking(GCMarker* gcmarker, T thing)
{
	return false;
}

template <>
bool
MustSkipMarking<JSObject*>(GCMarker* gcmarker, JSObject* obj)
{
	return false;
}


/*** Inline, Eager GC Marking *********************************************************************/

// Each of the eager, inline marking paths is directly preceeded by the
// out-of-line, generic tracing code for comparison. Both paths must end up
// traversing equivalent subgraphs.

void
LazyScript::traceChildren(JSTracer* trc)
{
    if (script_)
        TraceWeakEdge(trc, &script_, "script");

    if (function_)
        TraceEdge(trc, &function_, "function");

    if (sourceObject_)
        TraceEdge(trc, &sourceObject_, "sourceObject");

    if (enclosingScope_)
        TraceEdge(trc, &enclosingScope_, "enclosingScope");

    // We rely on the fact that atoms are always tenured.
    JSAtom** closedOverBindings = this->closedOverBindings();
    for (auto i : IntegerRange(numClosedOverBindings())) {
        if (closedOverBindings[i])
            TraceManuallyBarrieredEdge(trc, &closedOverBindings[i], "closedOverBinding");
    }

    GCPtrFunction* innerFunctions = this->innerFunctions();
    for (auto i : IntegerRange(numInnerFunctions()))
        TraceEdge(trc, &innerFunctions[i], "lazyScriptInnerFunction");
}

void
Shape::traceChildren(JSTracer* trc)
{
    TraceEdge(trc, &base_, "base");
    TraceEdge(trc, &propidRef(), "propid");
    if (parent)
        TraceEdge(trc, &parent, "parent");

    if (hasGetterObject())
        TraceManuallyBarrieredEdge(trc, &asAccessorShape().getterObj, "getter");
    if (hasSetterObject())
        TraceManuallyBarrieredEdge(trc, &asAccessorShape().setterObj, "setter");
}

void
JSString::traceChildren(JSTracer* trc)
{
    if (hasBase())
        traceBase(trc);
    else if (isRope())
        asRope().traceChildren(trc);
}

void
JSString::traceBase(JSTracer* trc)
{
    MOZ_ASSERT(hasBase());
    TraceManuallyBarrieredEdge(trc, &d.s.u3.base, "base");
}

void
JSRope::traceChildren(JSTracer* trc) {
    js::TraceManuallyBarrieredEdge(trc, &d.s.u2.left, "left child");
    js::TraceManuallyBarrieredEdge(trc, &d.s.u3.right, "right child");
}

static inline void
TraceBindingNames(JSTracer* trc, BindingName* names, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++) {
        JSAtom* name = names[i].name();
        MOZ_ASSERT(name);
        TraceManuallyBarrieredEdge(trc, &name, "scope name");
    }
};
static inline void
TraceNullableBindingNames(JSTracer* trc, BindingName* names, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++) {
        if (JSAtom* name = names[i].name())
            TraceManuallyBarrieredEdge(trc, &name, "scope name");
    }
};
void
BindingName::trace(JSTracer* trc)
{
    if (JSAtom* atom = name())
        TraceManuallyBarrieredEdge(trc, &atom, "binding name");
}
void
BindingIter::trace(JSTracer* trc)
{
    TraceNullableBindingNames(trc, names_, length_);
}
void
LexicalScope::Data::trace(JSTracer* trc)
{
    TraceBindingNames(trc, names, length);
}
void
FunctionScope::Data::trace(JSTracer* trc)
{
    TraceNullableEdge(trc, &canonicalFunction, "scope canonical function");
    TraceNullableBindingNames(trc, names, length);
}
void
VarScope::Data::trace(JSTracer* trc)
{
    TraceBindingNames(trc, names, length);
}
void
GlobalScope::Data::trace(JSTracer* trc)
{
    TraceBindingNames(trc, names, length);
}
void
EvalScope::Data::trace(JSTracer* trc)
{
    TraceBindingNames(trc, names, length);
}
void
ModuleScope::Data::trace(JSTracer* trc)
{
    TraceNullableEdge(trc, &module, "scope module");
    TraceBindingNames(trc, names, length);
}
void
WasmInstanceScope::Data::trace(JSTracer* trc)
{
    TraceNullableEdge(trc, &instance, "wasm instance");
    TraceBindingNames(trc, names, length);
}
void
WasmFunctionScope::Data::trace(JSTracer* trc)
{
    TraceBindingNames(trc, names, length);
}
void
Scope::traceChildren(JSTracer* trc)
{
    TraceNullableEdge(trc, &enclosing_, "scope enclosing");
    TraceNullableEdge(trc, &environmentShape_, "scope env shape");
    switch (kind_) {
      case ScopeKind::Function:
        reinterpret_cast<FunctionScope::Data*>(data_)->trace(trc);
        break;
      case ScopeKind::FunctionBodyVar:
      case ScopeKind::ParameterExpressionVar:
        reinterpret_cast<VarScope::Data*>(data_)->trace(trc);
        break;
      case ScopeKind::Lexical:
      case ScopeKind::SimpleCatch:
      case ScopeKind::Catch:
      case ScopeKind::NamedLambda:
      case ScopeKind::StrictNamedLambda:
        reinterpret_cast<LexicalScope::Data*>(data_)->trace(trc);
        break;
      case ScopeKind::Global:
      case ScopeKind::NonSyntactic:
        reinterpret_cast<GlobalScope::Data*>(data_)->trace(trc);
        break;
      case ScopeKind::Eval:
      case ScopeKind::StrictEval:
        reinterpret_cast<EvalScope::Data*>(data_)->trace(trc);
        break;
      case ScopeKind::Module:
        reinterpret_cast<ModuleScope::Data*>(data_)->trace(trc);
        break;
      case ScopeKind::With:
        break;
    }
}

void
js::ObjectGroup::traceChildren(JSTracer* trc)
{
    unsigned count = getPropertyCount();
    for (unsigned i = 0; i < count; i++) {
        if (ObjectGroup::Property* prop = getProperty(i))
            TraceEdge(trc, &prop->id, "group_property");
    }

    if (proto().isObject())
        TraceEdge(trc, &proto(), "group_proto");

    if (trc->isMarkingTracer() || trc->isOmrMarkingTracer())
        compartment()->mark();

    if (JSObject* global = compartment()->unsafeUnbarrieredMaybeGlobal())
        TraceManuallyBarrieredEdge(trc, &global, "group_global");


    if (newScript())
        newScript()->trace(trc);

    if (maybePreliminaryObjects())
        maybePreliminaryObjects()->trace(trc);

    if (maybeUnboxedLayout())
        unboxedLayout().trace(trc);

    if (ObjectGroup* unboxedGroup = maybeOriginalUnboxedGroup()) {
        TraceManuallyBarrieredEdge(trc, &unboxedGroup, "group_original_unboxed_group");
        setOriginalUnboxedGroup(unboxedGroup);
    }

    if (JSObject* descr = maybeTypeDescr()) {
        TraceManuallyBarrieredEdge(trc, &descr, "group_type_descr");
        setTypeDescr(&descr->as<TypeDescr>());
    }

    if (JSObject* fun = maybeInterpretedFunction()) {
        TraceManuallyBarrieredEdge(trc, &fun, "group_function");
        setInterpretedFunction(&fun->as<JSFunction>());
    }
}

// Call the trace hook set on the object, if present. If further tracing of
// NativeObject fields is required, this will return the native object.
enum class CheckGeneration { DoChecks, NoChecks};
template <typename Functor, typename... Args>
static inline NativeObject*
CallTraceHook(Functor f, JSTracer* trc, JSObject* obj, CheckGeneration check, Args&&... args)
{
    const Class* clasp = obj->getClass();
    MOZ_ASSERT(clasp);
    MOZ_ASSERT(obj->isNative() == clasp->isNative());

    if (!clasp->hasTrace())
        return &obj->as<NativeObject>();

    if (clasp->isTrace(InlineTypedObject::obj_trace)) {
        Shape** pshape = obj->as<InlineTypedObject>().addressOfShapeFromGC();
        f(pshape, mozilla::Forward<Args>(args)...);

        InlineTypedObject& tobj = obj->as<InlineTypedObject>();
        if (tobj.typeDescr().hasTraceList()) {
            VisitTraceList(f, tobj.typeDescr().traceList(), tobj.inlineTypedMemForGC(),
                           mozilla::Forward<Args>(args)...);
        }

        return nullptr;
    }

    if (clasp == &UnboxedPlainObject::class_) {
        JSObject** pexpando = obj->as<UnboxedPlainObject>().addressOfExpando();
        if (*pexpando)
            f(pexpando, mozilla::Forward<Args>(args)...);

        UnboxedPlainObject& unboxed = obj->as<UnboxedPlainObject>();
        const UnboxedLayout& layout = check == CheckGeneration::DoChecks
                                      ? unboxed.layout()
                                      : unboxed.layoutDontCheckGeneration();
        if (layout.traceList()) {
            VisitTraceList(f, layout.traceList(), unboxed.data(),
                           mozilla::Forward<Args>(args)...);
        }

        return nullptr;
    }

    clasp->doTrace(trc, obj);

    if (!clasp->isNative())
        return nullptr;
    return &obj->as<NativeObject>();
}

template <typename F, typename... Args>
static void
VisitTraceList(F f, const int32_t* traceList, uint8_t* memory, Args&&... args)
{
    while (*traceList != -1) {
        f(reinterpret_cast<JSString**>(memory + *traceList), mozilla::Forward<Args>(args)...);
        traceList++;
    }
    traceList++;
    while (*traceList != -1) {
        JSObject** objp = reinterpret_cast<JSObject**>(memory + *traceList);
        if (*objp)
            f(objp, mozilla::Forward<Args>(args)...);
        traceList++;
    }
    traceList++;
    while (*traceList != -1) {
        f(reinterpret_cast<Value*>(memory + *traceList), mozilla::Forward<Args>(args)...);
        traceList++;
    }
}


/*** Tenuring Tracer *****************************************************************************/

namespace js {
template <typename T>
void
TenuringTracer::traverse(T** tp)
{
}

template <>
void
TenuringTracer::traverse(JSObject** objp)
{
}

template <typename S>
struct TenuringTraversalFunctor : public IdentityDefaultAdaptor<S> {
    template <typename T> S operator()(T* t, TenuringTracer* trc) {
        trc->traverse(&t);
        return js::gc::RewrapTaggedPointer<S, T>::wrap(t);
    }
};

template <typename T>
void
TenuringTracer::traverse(T* thingp)
{
    *thingp = DispatchTyped(TenuringTraversalFunctor<T>(), *thingp, this);
}
} // namespace js

static inline void
TraceWholeCell(TenuringTracer& mover, JSObject* object)
{
    mover.traceObject(object);

    // Additionally trace the expando object attached to any unboxed plain
    // objects. Baseline and Ion can write properties to the expando while
    // only adding a post barrier to the owning unboxed object. Note that
    // it isn't possible for a nursery unboxed object to have a tenured
    // expando, so that adding a post barrier on the original object will
    // capture any tenured->nursery edges in the expando as well.

    if (object->is<UnboxedPlainObject>()) {
        if (UnboxedExpandoObject* expando = object->as<UnboxedPlainObject>().maybeExpando())
            expando->traceChildren(&mover);
    }
}

static inline void
TraceWholeCell(TenuringTracer& mover, JSScript* script)
{
    script->traceChildren(&mover);
}

static inline void
TraceWholeCell(TenuringTracer& mover, jit::JitCode* jitcode)
{
    jitcode->traceChildren(&mover);
}

template <typename T>
static void
TraceBufferedCells(TenuringTracer& mover)
{
}


/*** IsMarked / IsAboutToBeFinalized **************************************************************/

template <typename T>
static inline void
CheckIsMarkedThing(T* thingp)
{
}

static bool
IsMarkedInternalCommon(void* thingp)
{
	MM_EnvironmentBase *env = MM_EnvironmentBase::getEnvironment(Nursery::omrVMThread);
	return ((MM_ParallelGlobalGC*)env->getExtensions()->getGlobalCollector())->getMarkingScheme()->isMarked((omrobjectptr_t)(thingp));
}

bool
js::gc::IsAboutToBeFinalizedDuringSweep(TenuredCell& tenured)
{
    return IsMarkedCell(&tenured);
}

template <typename T>
static bool
IsAboutToBeFinalizedInternal(T** thingp)
{
    return !IsMarkedInternalCommon((void*)(*thingp));
}

template <typename S>
struct IsAboutToBeFinalizedFunctor : public IdentityDefaultAdaptor<S> {
    template <typename T> S operator()(T* t, bool* rv) {
        *rv = IsAboutToBeFinalizedInternal(&t);
        return js::gc::RewrapTaggedPointer<S, T>::wrap(t);
    }
};

template <typename T>
static bool
IsAboutToBeFinalizedInternal(T* thingp)
{
    bool rv = false;
    *thingp = DispatchTyped(IsAboutToBeFinalizedFunctor<T>(), *thingp, &rv);
    return rv;
}

namespace js {
namespace gc {

bool
IsMarkedCell(const TenuredCell* const thingp)
{
	return IsMarkedUnbarriered(nullptr, thingp);
}

template <typename T>
bool
IsMarkedUnbarriered(JSRuntime* rt, T* thingp)
{
    return IsMarkedInternalCommon((void *)thingp);
}

template <typename T>
bool
IsMarked(JSRuntime* rt, WriteBarrieredBase<T>* thingp)
{
    return IsMarkedInternalCommon((void *)thingp->unsafeUnbarrieredForTracing());
}

template <typename T>
bool
IsAboutToBeFinalizedUnbarriered(T* thingp)
{
    return IsAboutToBeFinalizedInternal(ConvertToBase(thingp));
}

template <typename T>
bool
IsAboutToBeFinalized(WriteBarrieredBase<T>* thingp)
{
    return IsAboutToBeFinalizedInternal(ConvertToBase(thingp->unsafeUnbarrieredForTracing()));
}

template <typename T>
bool
IsAboutToBeFinalized(ReadBarrieredBase<T>* thingp)
{
    return IsAboutToBeFinalizedInternal(ConvertToBase(thingp->unsafeUnbarrieredForTracing()));
}

template <typename T>
JS_PUBLIC_API(bool)
EdgeNeedsSweep(JS::Heap<T>* thingp)
{
    return IsAboutToBeFinalizedInternal(ConvertToBase(thingp->unsafeGet()));
}

template <typename T>
JS_PUBLIC_API(bool)
EdgeNeedsSweepUnbarrieredSlow(T* thingp)
{
    return IsAboutToBeFinalizedInternal(ConvertToBase(thingp));
}

// Instantiate a copy of the Tracing templates for each derived type.
#define INSTANTIATE_ALL_VALID_TRACE_FUNCTIONS(type) \
    template bool IsMarkedUnbarriered<type>(JSRuntime*, type*);                \
    template bool IsMarked<type>(JSRuntime*, WriteBarrieredBase<type>*); \
    template bool IsAboutToBeFinalizedUnbarriered<type>(type*); \
    template bool IsAboutToBeFinalized<type>(WriteBarrieredBase<type>*); \
    template bool IsAboutToBeFinalized<type>(ReadBarrieredBase<type>*);
#define INSTANTIATE_ALL_VALID_HEAP_TRACE_FUNCTIONS(type) \
    template JS_PUBLIC_API(bool) EdgeNeedsSweep<type>(JS::Heap<type>*); \
    template JS_PUBLIC_API(bool) EdgeNeedsSweepUnbarrieredSlow<type>(type*);
FOR_EACH_GC_POINTER_TYPE(INSTANTIATE_ALL_VALID_TRACE_FUNCTIONS)
FOR_EACH_PUBLIC_GC_POINTER_TYPE(INSTANTIATE_ALL_VALID_HEAP_TRACE_FUNCTIONS)
FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(INSTANTIATE_ALL_VALID_HEAP_TRACE_FUNCTIONS)
#undef INSTANTIATE_ALL_VALID_TRACE_FUNCTIONS

} /* namespace gc */
} /* namespace js */



JS_FRIEND_API(bool)
JS::UnmarkGrayGCThingRecursively(JS::GCCellPtr thing)
{
    return true;
}

bool
js::UnmarkGrayShapeRecursively(Shape* shape)
{
    return JS::UnmarkGrayGCThingRecursively(JS::GCCellPtr(shape));
}

