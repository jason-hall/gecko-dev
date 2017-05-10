/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * This code implements an incremental mark-and-sweep garbage collector, with
 * most sweeping carried out in the background on a parallel thread.
 *
 * Full vs. zone GC
 * ----------------
 *
 * The collector can collect all zones at once, or a subset. These types of
 * collection are referred to as a full GC and a zone GC respectively.
 *
 * It is possible for an incremental collection that started out as a full GC to
 * become a zone GC if new zones are created during the course of the
 * collection.
 *
 * Incremental collection
 * ----------------------
 *
 * For a collection to be carried out incrementally the following conditions
 * must be met:
 *  - the collection must be run by calling js::GCSlice() rather than js::GC()
 *  - the GC mode must have been set to JSGC_MODE_INCREMENTAL with
 *    JS_SetGCParameter()
 *  - no thread may have an AutoKeepAtoms instance on the stack
 *
 * The last condition is an engine-internal mechanism to ensure that incremental
 * collection is not carried out without the correct barriers being implemented.
 * For more information see 'Incremental marking' below.
 *
 * If the collection is not incremental, all foreground activity happens inside
 * a single call to GC() or GCSlice(). However the collection is not complete
 * until the background sweeping activity has finished.
 *
 * An incremental collection proceeds as a series of slices, interleaved with
 * mutator activity, i.e. running JavaScript code. Slices are limited by a time
 * budget. The slice finishes as soon as possible after the requested time has
 * passed.
 *
 * Collector states
 * ----------------
 *
 * The collector proceeds through the following states, the current state being
 * held in JSRuntime::gcIncrementalState:
 *
 *  - MarkRoots  - marks the stack and other roots
 *  - Mark       - incrementally marks reachable things
 *  - Sweep      - sweeps zones in groups and continues marking unswept zones
 *  - Finalize   - performs background finalization, concurrent with mutator
 *  - Compact    - incrementally compacts by zone
 *  - Decommit   - performs background decommit and chunk removal
 *
 * The MarkRoots activity always takes place in the first slice. The next two
 * states can take place over one or more slices.
 *
 * In other words an incremental collection proceeds like this:
 *
 * Slice 1:   MarkRoots:  Roots pushed onto the mark stack.
 *            Mark:       The mark stack is processed by popping an element,
 *                        marking it, and pushing its children.
 *
 *          ... JS code runs ...
 *
 * Slice 2:   Mark:       More mark stack processing.
 *
 *          ... JS code runs ...
 *
 * Slice n-1: Mark:       More mark stack processing.
 *
 *          ... JS code runs ...
 *
 * Slice n:   Mark:       Mark stack is completely drained.
 *            Sweep:      Select first group of zones to sweep and sweep them.
 *
 *          ... JS code runs ...
 *
 * Slice n+1: Sweep:      Mark objects in unswept zones that were newly
 *                        identified as alive (see below). Then sweep more zone
 *                        sweep groups.
 *
 *          ... JS code runs ...
 *
 * Slice n+2: Sweep:      Mark objects in unswept zones that were newly
 *                        identified as alive. Then sweep more zones.
 *
 *          ... JS code runs ...
 *
 * Slice m:   Sweep:      Sweeping is finished, and background sweeping
 *                        started on the helper thread.
 *
 *          ... JS code runs, remaining sweeping done on background thread ...
 *
 * When background sweeping finishes the GC is complete.
 *
 * Incremental marking
 * -------------------
 *
 * Incremental collection requires close collaboration with the mutator (i.e.,
 * JS code) to guarantee correctness.
 *
 *  - During an incremental GC, if a memory location (except a root) is written
 *    to, then the value it previously held must be marked. Write barriers
 *    ensure this.
 *
 *  - Any object that is allocated during incremental GC must start out marked.
 *
 *  - Roots are marked in the first slice and hence don't need write barriers.
 *    Roots are things like the C stack and the VM stack.
 *
 * The problem that write barriers solve is that between slices the mutator can
 * change the object graph. We must ensure that it cannot do this in such a way
 * that makes us fail to mark a reachable object (marking an unreachable object
 * is tolerable).
 *
 * We use a snapshot-at-the-beginning algorithm to do this. This means that we
 * promise to mark at least everything that is reachable at the beginning of
 * collection. To implement it we mark the old contents of every non-root memory
 * location written to by the mutator while the collection is in progress, using
 * write barriers. This is described in gc/Barrier.h.
 *
 * Incremental sweeping
 * --------------------
 *
 * Sweeping is difficult to do incrementally because object finalizers must be
 * run at the start of sweeping, before any mutator code runs. The reason is
 * that some objects use their finalizers to remove themselves from caches. If
 * mutator code was allowed to run after the start of sweeping, it could observe
 * the state of the cache and create a new reference to an object that was just
 * about to be destroyed.
 *
 * Sweeping all finalizable objects in one go would introduce long pauses, so
 * instead sweeping broken up into groups of zones. Zones which are not yet
 * being swept are still marked, so the issue above does not apply.
 *
 * The order of sweeping is restricted by cross compartment pointers - for
 * example say that object |a| from zone A points to object |b| in zone B and
 * neither object was marked when we transitioned to the Sweep phase. Imagine we
 * sweep B first and then return to the mutator. It's possible that the mutator
 * could cause |a| to become alive through a read barrier (perhaps it was a
 * shape that was accessed via a shape table). Then we would need to mark |b|,
 * which |a| points to, but |b| has already been swept.
 *
 * So if there is such a pointer then marking of zone B must not finish before
 * marking of zone A.  Pointers which form a cycle between zones therefore
 * restrict those zones to being swept at the same time, and these are found
 * using Tarjan's algorithm for finding the strongly connected components of a
 * graph.
 *
 * GC things without finalizers, and things with finalizers that are able to run
 * in the background, are swept on the background thread. This accounts for most
 * of the sweeping work.
 *
 * Reset
 * -----
 *
 * During incremental collection it is possible, although unlikely, for
 * conditions to change such that incremental collection is no longer safe. In
 * this case, the collection is 'reset' by ResetIncrementalGC(). If we are in
 * the mark state, this just stops marking, but if we have started sweeping
 * already, we continue until we have swept the current sweep group. Following a
 * reset, a new non-incremental collection is started.
 *
 * Compacting GC
 * -------------
 *
 * Compacting GC happens at the end of a major GC as part of the last slice.
 * There are three parts:
 *
 *  - Arenas are selected for compaction.
 *  - The contents of those arenas are moved to new arenas.
 *  - All references to moved things are updated.
 *
 * Collecting Atoms
 * ----------------
 *
 * Atoms are collected differently from other GC things. They are contained in
 * a special zone and things in other zones may have pointers to them that are
 * not recorded in the cross compartment pointer map. Each zone holds a bitmap
 * with the atoms it might be keeping alive, and atoms are only collected if
 * they are not included in any zone's atom bitmap. See AtomMarking.cpp for how
 * this bitmap is managed.
 */

#include "jsgcinlines.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MacroForEach.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Move.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/SizePrintfMacros.h"
#include "mozilla/TimeStamp.h"

#include <ctype.h>
#include <string.h>
#ifndef XP_WIN
# include <sys/mman.h>
# include <unistd.h>
#endif

#include "jsapi.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jscompartment.h"
#include "jsfriendapi.h"
#include "jsobj.h"
#include "jsprf.h"
#include "jsscript.h"
#include "jstypes.h"
#include "jsutil.h"
#include "jswatchpoint.h"
#include "jsweakmap.h"
#ifdef XP_WIN
# include "jswin.h"
#endif

#include "gc/FindSCCs.h"
#include "gc/GCInternals.h"
#include "gc/GCTrace.h"
#include "gc/Marking.h"
#include "gc/Memory.h"
#include "gc/Policy.h"
#include "jit/BaselineJIT.h"
#include "jit/IonCode.h"
#include "jit/JitcodeMap.h"
#include "js/SliceBudget.h"
#include "proxy/DeadObjectProxy.h"
#include "vm/Debugger.h"
#include "vm/GeckoProfiler.h"
#include "vm/ProxyObject.h"
#include "vm/Shape.h"
#include "vm/String.h"
#include "vm/Symbol.h"
#include "vm/Time.h"
#include "vm/TraceLogging.h"
#include "vm/WrapperObject.h"

#include "jsobjinlines.h"
#include "jsscriptinlines.h"

#include "gc/Heap-inl.h"
#include "gc/Nursery-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/Stack-inl.h"
#include "vm/String-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::ArrayLength;
using mozilla::Get;
using mozilla::HashCodeScrambler;
using mozilla::Maybe;
using mozilla::Swap;

using JS::AutoGCRooter;

/* Increase the IGC marking slice time if we are in highFrequencyGC mode. */
static const int IGC_MARK_SLICE_MULTIPLIER = 2;

const AllocKind gc::slotsToThingKind[] = {
    /*  0 */ AllocKind::OBJECT0,  AllocKind::OBJECT2,  AllocKind::OBJECT2,  AllocKind::OBJECT4,
    /*  4 */ AllocKind::OBJECT4,  AllocKind::OBJECT8,  AllocKind::OBJECT8,  AllocKind::OBJECT8,
    /*  8 */ AllocKind::OBJECT8,  AllocKind::OBJECT12, AllocKind::OBJECT12, AllocKind::OBJECT12,
    /* 12 */ AllocKind::OBJECT12, AllocKind::OBJECT16, AllocKind::OBJECT16, AllocKind::OBJECT16,
    /* 16 */ AllocKind::OBJECT16
};

#ifdef OMR // Sizes

 const uint32_t OmrGcHelper::thingSizes[] = {
 #define EXPAND_THING_SIZE(allocKind, traceKind, type, sizedType) \
     sizeof(sizedType),
 FOR_EACH_ALLOCKIND(EXPAND_THING_SIZE)
 #undef EXPAND_THING_SIZE
 };

#else // OMR Sizes

 const uint32_t Arena::ThingSizes[] = {
 #define EXPAND_THING_SIZE(allocKind, traceKind, type, sizedType) \
     sizeof(sizedType),
 FOR_EACH_ALLOCKIND(EXPAND_THING_SIZE)
 #undef EXPAND_THING_SIZE
 };

 #define COUNT(type) 1

 const uint32_t Arena::ThingsPerArena[] = {
 #define EXPAND_THINGS_PER_ARENA(allocKind, traceKind, type, sizedType) \
     COUNT(sizedType),
 FOR_EACH_ALLOCKIND(EXPAND_THINGS_PER_ARENA)
 #undef EXPAND_THINGS_PER_ARENA
 };

#endif // ! OMR Sizes


struct js::gc::FinalizePhase
{
    gcstats::Phase statsPhase;
    AllocKinds kinds;
};

/*
 * Finalization order for objects swept incrementally on the active thread.
 */
static const FinalizePhase ForegroundObjectFinalizePhase = {
    gcstats::PHASE_SWEEP_OBJECT, {
        AllocKind::OBJECT0,
        AllocKind::OBJECT2,
        AllocKind::OBJECT4,
        AllocKind::OBJECT8,
        AllocKind::OBJECT12,
        AllocKind::OBJECT16
    }
};

// Incremental sweeping is controlled by a list of actions that describe what
// happens and in what order. Due to the incremental nature of sweeping an
// action does not necessarily run to completion so the current state is tracked
// in the GCRuntime by the performSweepActions() method.
//
// Actions are performed in phases run per sweep group, and each action is run
// for every zone in the group, i.e. as if by the following pseudocode:
//
//   for each sweep group:
//     for each phase:
//       for each zone in sweep group:
//         for each action in phase:
//           perform_action

struct SweepAction
{
    using Func = IncrementalProgress (*)(GCRuntime* gc, FreeOp* fop, Zone* zone,
                                         SliceBudget& budget, AllocKind kind);

    Func func;
    AllocKind kind;

    SweepAction(Func func, AllocKind kind) : func(func), kind(kind) {}
};

using SweepActionVector = Vector<SweepAction, 0, SystemAllocPolicy>;
using SweepPhaseVector = Vector<SweepActionVector, 0, SystemAllocPolicy>;

static SweepPhaseVector SweepPhases;

bool
js::gc::InitializeStaticData()
{
    return GCRuntime::initializeSweepActions();
}

GCRuntime::GCRuntime(JSRuntime* rt) :
    rt(rt),
    systemZone(nullptr),
    systemZoneGroup(nullptr),
    atomsZone(nullptr),
    stats_(rt),
    marker(rt),
    nextCellUniqueId_(LargestTaggedNullCellPointer + 1), // Ensure disjoint from null tagged pointers.
    numActiveZoneIters(0),
    number(0),
    isFull(false),
    incrementalState(gc::State::NotActive),
    sweepGroups(nullptr),
    currentSweepGroup(nullptr),
    sweepPhaseIndex(0),
    sweepZone(nullptr),
    sweepActionIndex(0),
    abortSweepAfterCurrentGroup(false),
    compactingEnabled(true),
    poked(false),
#ifdef JS_GC_ZEAL
    zealModeBits(0),
    nextScheduled(0),
#endif
    lock(mutexid::GCLock),
	nursery_(rt),
	enabled(0)
{
}

#ifdef JS_GC_ZEAL

void
GCRuntime::getZealBits(uint32_t* zealBits, uint32_t* frequency, uint32_t* scheduled)
{
}

const char* gc::ZealModeHelpText =
    "  Specifies how zealous the garbage collector should be. Some of these modes can\n"
    "  be set simultaneously, by passing multiple level options, e.g. \"2;4\" will activate\n"
    "  both modes 2 and 4. Modes can be specified by name or number.\n"
    "  \n"
    "  Values:\n"
    "    0: (None) Normal amount of collection (resets all modes)\n"
    "    1: (Poke) Collect when roots are added or removed\n"
    "    2: (Alloc) Collect when every N allocations (default: 100)\n"
    "    3: (FrameGC) Collect when the window paints (browser only)\n"
    "    4: (VerifierPre) Verify pre write barriers between instructions\n"
    "    5: (FrameVerifierPre) Verify pre write barriers between paints\n"
    "    6: (StackRooting) Verify stack rooting\n"
    "    7: (GenerationalGC) Collect the nursery every N nursery allocations\n"
    "    8: (IncrementalRootsThenFinish) Incremental GC in two slices: 1) mark roots 2) finish collection\n"
    "    9: (IncrementalMarkAllThenFinish) Incremental GC in two slices: 1) mark all 2) new marking and finish\n"
    "   10: (IncrementalMultipleSlices) Incremental GC in multiple slices\n"
    "   11: (IncrementalMarkingValidator) Verify incremental marking\n"
    "   12: (ElementsBarrier) Always use the individual element post-write barrier, regardless of elements size\n"
    "   13: (CheckHashTablesOnMinorGC) Check internal hashtables on minor GC\n"
    "   14: (Compact) Perform a shrinking collection every N allocations\n"
    "   15: (CheckHeapAfterGC) Walk the heap to check its integrity after every GC\n"
    "   16: (CheckNursery) Check nursery integrity on minor GC\n"
    "   17: (IncrementalSweepThenFinish) Incremental GC in two slices: 1) start sweeping 2) finish collection\n";

// The set of zeal modes that control incremental slices. These modes are
// mutually exclusive.
static const mozilla::EnumSet<ZealMode> IncrementalSliceZealModes = {
    ZealMode::IncrementalRootsThenFinish,
    ZealMode::IncrementalMarkAllThenFinish,
    ZealMode::IncrementalMultipleSlices,
    ZealMode::IncrementalSweepThenFinish
};

void
GCRuntime::setZeal(uint8_t zeal, uint32_t frequency)
{
}

void
GCRuntime::setNextScheduled(uint32_t count)
{
}

bool
GCRuntime::parseAndSetZeal(const char* str)
{
    return true;
}

static const char*
AllocKindName(AllocKind kind)
{
    static const char* names[] = {
#define EXPAND_THING_NAME(allocKind, _1, _2, _3) \
        #allocKind,
FOR_EACH_ALLOCKIND(EXPAND_THING_NAME)
#undef EXPAND_THING_NAME
    };
    static_assert(ArrayLength(names) == size_t(AllocKind::LIMIT),
                  "names array should have an entry for every AllocKind");

    size_t i = size_t(kind);
    MOZ_ASSERT(i < ArrayLength(names));
    return names[i];
}

#endif // JS_GC_ZEAL

/*
 * Lifetime in number of major GCs for type sets attached to scripts containing
 * observed types.
 */
static const uint64_t JIT_SCRIPT_RELEASE_TYPES_PERIOD = 20;

bool
GCRuntime::init(uint32_t maxbytes, uint32_t maxNurseryBytes)
{
    MOZ_ASSERT(SystemPageSize());

    if (!rootsHash.ref().init(256))
        return false;

    {
        AutoLockGC lock(rt);

        if (!nursery().init(maxNurseryBytes, lock))
            return false;
    }
}

void
GCRuntime::finish()
{
}

bool
GCRuntime::setParameter(JSGCParamKey key, uint32_t value, AutoLockGC& lock)
{
    return true;
}

uint32_t
GCRuntime::getParameter(JSGCParamKey key, const AutoLockGC& lock)
{
    return 0;
}

bool
GCRuntime::addBlackRootsTracer(JSTraceDataOp traceOp, void* data)
{
    return true;
}

void
GCRuntime::removeBlackRootsTracer(JSTraceDataOp traceOp, void* data)
{
}

void
GCRuntime::setGrayRootsTracer(JSTraceDataOp traceOp, void* data)
{
}

void
GCRuntime::setGCCallback(JSGCCallback callback, void* data)
{
}

void
GCRuntime::callGCCallback(JSGCStatus status) const
{
}

void
GCRuntime::setObjectsTenuredCallback(JSObjectsTenuredCallback callback,
                                     void* data)
{
}

namespace {

class AutoNotifyGCActivity {
  public:
    explicit AutoNotifyGCActivity(GCRuntime& gc) {
    }
    ~AutoNotifyGCActivity() {\
    }
};

} // (anon)

bool
GCRuntime::addFinalizeCallback(JSFinalizeCallback callback, void* data)
{
    return finalizeCallbacks.ref().append(Callback<JSFinalizeCallback>(callback, data));
}

void
GCRuntime::removeFinalizeCallback(JSFinalizeCallback callback)
{
    for (Callback<JSFinalizeCallback>* p = finalizeCallbacks.ref().begin();
         p < finalizeCallbacks.ref().end(); p++)
    {
        if (p->op == callback) {
            finalizeCallbacks.ref().erase(p);
            break;
        }
    }
}

void
GCRuntime::callFinalizeCallbacks(FreeOp* fop, JSFinalizeStatus status) const
{
    for (auto& p : finalizeCallbacks.ref())
        p.op(fop, status, !isFull, p.data);
}

bool
GCRuntime::addWeakPointerZonesCallback(JSWeakPointerZonesCallback callback, void* data)
{
    return true;
}

void
GCRuntime::removeWeakPointerZonesCallback(JSWeakPointerZonesCallback callback)
{
}

bool
GCRuntime::addWeakPointerCompartmentCallback(JSWeakPointerCompartmentCallback callback, void* data)
{
    return true;
}

void
GCRuntime::removeWeakPointerCompartmentCallback(JSWeakPointerCompartmentCallback callback)
{
}

bool
GCRuntime::addRoot(Value* vp, const char* name)
{
    return rootsHash.ref().put(vp, name);
}

void
GCRuntime::removeRoot(Value* vp)
{
    rootsHash.ref().remove(vp);
}

extern JS_FRIEND_API(bool)
js::AddRawValueRoot(JSContext* cx, Value* vp, const char* name)
{
    MOZ_ASSERT(vp);
    MOZ_ASSERT(name);
    bool ok = cx->runtime()->gc.addRoot(vp, name);
    if (!ok)
        JS_ReportOutOfMemory(cx);
    return ok;
}

extern JS_FRIEND_API(void)
js::RemoveRawValueRoot(JSContext* cx, Value* vp)
{
    cx->runtime()->gc.removeRoot(vp);
}

void
GCRuntime::updateMallocCounter(JS::Zone* zone, size_t nbytes)
{
}

// OMRTODO: Delete
void
GCMarker::delayMarkingArena()
{
}

void
GCMarker::delayMarkingChildren(const void* thing)
{
}

/* Compacting GC */

AutoDisableCompactingGC::AutoDisableCompactingGC(JSContext* cx)
  : cx(cx)
{
    cx->runtime()->gc.disable();
}

AutoDisableCompactingGC::~AutoDisableCompactingGC()
{
    cx->runtime()->gc.enable();
}

static const AllocKind AllocKindsToRelocate[] = {
    AllocKind::FUNCTION,
    AllocKind::FUNCTION_EXTENDED,
    AllocKind::OBJECT0,
    AllocKind::OBJECT0_BACKGROUND,
    AllocKind::OBJECT2,
    AllocKind::OBJECT2_BACKGROUND,
    AllocKind::OBJECT4,
    AllocKind::OBJECT4_BACKGROUND,
    AllocKind::OBJECT8,
    AllocKind::OBJECT8_BACKGROUND,
    AllocKind::OBJECT12,
    AllocKind::OBJECT12_BACKGROUND,
    AllocKind::OBJECT16,
    AllocKind::OBJECT16_BACKGROUND,
    AllocKind::SCRIPT,
    AllocKind::LAZY_SCRIPT,
    AllocKind::SHAPE,
    AllocKind::ACCESSOR_SHAPE,
    AllocKind::BASE_SHAPE,
    AllocKind::FAT_INLINE_STRING,
    AllocKind::STRING,
    AllocKind::EXTERNAL_STRING,
    AllocKind::FAT_INLINE_ATOM,
    AllocKind::ATOM,
    AllocKind::SCOPE,
    AllocKind::REGEXP_SHARED
};

static bool
ShouldRelocateAllArenas(JS::gcreason::Reason reason)
{
    return false;
}

#ifdef DEBUG
inline bool
PtrIsInRange(const void* ptr, const void* start, size_t length)
{
    return uintptr_t(ptr) - uintptr_t(start) < length;
}
#endif

static inline bool
ShouldProtectRelocatedArenas(JS::gcreason::Reason reason)
{
    return false;
}

static const size_t MinCellUpdateBackgroundTasks = 2;
static const size_t MaxCellUpdateBackgroundTasks = 8;

// After cells have been relocated any pointers to a cell's old locations must
// be updated to point to the new location.  This happens by iterating through
// all cells in heap and tracing their children (non-recursively) to update
// them.
//
// This is complicated by the fact that updating a GC thing sometimes depends on
// making use of other GC things.  After a moving GC these things may not be in
// a valid state since they may contain pointers which have not been updated
// yet.
//
// The main dependencies are:
//
//   - Updating a JSObject makes use of its shape
//   - Updating a typed object makes use of its type descriptor object
//
// This means we require at least three phases for update:
//
//  1) shapes
//  2) typed object type descriptor objects
//  3) all other objects
//
// Since we want to minimize the number of phases, we put everything else into
// the first phase and label it the 'misc' phase.

SliceBudget::SliceBudget()
  : timeBudget(UnlimitedTimeBudget), workBudget(UnlimitedWorkBudget)
{
}

SliceBudget::SliceBudget(TimeBudget time)
  : timeBudget(time), workBudget(UnlimitedWorkBudget)
{
}

SliceBudget::SliceBudget(WorkBudget work)
  : timeBudget(UnlimitedTimeBudget), workBudget(work)
{
}

int
SliceBudget::describe(char* buffer, size_t maxlen) const
{
	return snprintf(buffer, maxlen, " ");
}

bool
SliceBudget::checkOverBudget()
{
    return false;
}

void
GCRuntime::maybeGC(Zone* zone)
{
}

void
GCHelperState::finish()
{
}

void
GCHelperState::work()
{
}

void
GCRuntime::freeUnusedLifoBlocksAfterSweeping(LifoAlloc* lifo)
{
}

void
GCRuntime::freeAllLifoBlocksAfterSweeping(LifoAlloc* lifo)
{
}

void
GCRuntime::freeAllLifoBlocksAfterMinorGC(LifoAlloc* lifo)
{
}

void
GCHelperState::waitBackgroundSweepEnd()
{
}


struct IsAboutToBeFinalizedFunctor {
    template <typename T> bool operator()(Cell** t) {
        mozilla::DebugOnly<const Cell*> prior = *t;
        bool result = IsAboutToBeFinalizedUnbarriered(reinterpret_cast<T**>(t));
        // Sweep should not have to deal with moved pointers, since moving GC
        // handles updating the UID table manually.
        MOZ_ASSERT(*t == prior);
        return result;
    }
};

/* static */ bool
UniqueIdGCPolicy::needsSweep(Cell** cell, uint64_t*)
{
    return DispatchTraceKindTyped(IsAboutToBeFinalizedFunctor(), (*cell)->getTraceKind(), cell);
}

void
JS::Zone::sweepUniqueIds(js::FreeOp* fop)
{
    uniqueIds().sweep();
}

#ifdef DEBUG
static const char*
AllocKindToAscii(AllocKind kind)
{
    switch(kind) {
#define MAKE_CASE(allocKind, traceKind, type, sizedType) \
      case AllocKind:: allocKind: return #allocKind;
FOR_EACH_ALLOCKIND(MAKE_CASE)
#undef MAKE_CASE

      default:
        MOZ_CRASH("Unknown AllocKind in AllocKindToAscii");
    }
}
#endif // DEBUG

#ifdef DEBUG
class CompartmentCheckTracer : public JS::CallbackTracer
{
};

namespace {
struct IsDestComparatorFunctor {
    JS::GCCellPtr dst_;
    explicit IsDestComparatorFunctor(JS::GCCellPtr dst) : dst_(dst) {}

    template <typename T> bool operator()(T* t) { return (*t) == dst_.asCell(); }
};
} // namespace (anonymous)

static bool
InCrossCompartmentMap(JSObject* src, JS::GCCellPtr dst)
{
    return false;
}

struct MaybeCompartmentFunctor {
    template <typename T> JSCompartment* operator()(T* t) { return t->maybeCompartment(); }
};

#endif

#ifdef JS_GC_ZEAL

class js::gc::MarkingValidator
{
  public:
    explicit MarkingValidator(GCRuntime* gc);
    ~MarkingValidator();
    void nonIncrementalMark(AutoLockForExclusiveAccess& lock);
    void validate();
};

js::gc::MarkingValidator::MarkingValidator(GCRuntime* gc)
{}

js::gc::MarkingValidator::~MarkingValidator()
{
}

void
js::gc::MarkingValidator::nonIncrementalMark(AutoLockForExclusiveAccess& lock)
{
}

void
js::gc::MarkingValidator::validate()
{
}

#endif // JS_GC_ZEAL

/*
 * Group zones that must be swept at the same time.
 *
 * If compartment A has an edge to an unmarked object in compartment B, then we
 * must not sweep A in a later slice than we sweep B. That's because a write
 * barrier in A could lead to the unmarked object in B becoming marked.
 * However, if we had already swept that object, we would be in trouble.
 *
 * If we consider these dependencies as a graph, then all the compartments in
 * any strongly-connected component of this graph must be swept in the same
 * slice.
 *
 * Tarjan's algorithm is used to calculate the components.
 */
namespace {
struct AddOutgoingEdgeFunctor {
    AddOutgoingEdgeFunctor(bool needsEdge, ZoneComponentFinder& finder)
    {}

    template <typename T>
    void operator()(T tp) {
    }
};
} // namespace (anonymous)

void
JSCompartment::findOutgoingEdges(ZoneComponentFinder& finder)
{
}

bool
JSCompartment::findDeadProxyZoneEdges(bool* foundAny)
{
    return true;
}

/*
 * Gray marking:
 *
 * At the end of collection, anything reachable from a gray root that has not
 * otherwise been marked black must be marked gray.
 *
 * This means that when marking things gray we must not allow marking to leave
 * the current compartment group, as that could result in things being marked
 * grey when they might subsequently be marked black.  To achieve this, when we
 * find a cross compartment pointer we don't mark the referent but add it to a
 * singly-linked list of incoming gray pointers that is stored with each
 * compartment.
 *
 * The list head is stored in JSCompartment::gcIncomingGrayPointers and contains
 * cross compartment wrapper objects. The next pointer is stored in the second
 * extra slot of the cross compartment wrapper.
 *
 * The list is created during gray marking when one of the
 * MarkCrossCompartmentXXX functions is called for a pointer that leaves the
 * current compartent group.  This calls DelayCrossCompartmentGrayMarking to
 * push the referring object onto the list.
 *
 * The list is traversed and then unlinked in
 * MarkIncomingCrossCompartmentPointers.
 */

#ifdef DEBUG
static void
AssertNotOnGrayList(JSObject* obj)
{
}
#endif

void
js::NotifyGCNukeWrapper(JSObject* obj)
{
}

enum {
    JS_GC_SWAP_OBJECT_A_REMOVED = 1 << 0,
    JS_GC_SWAP_OBJECT_B_REMOVED = 1 << 1
};

unsigned
js::NotifyGCPreSwap(JSObject* a, JSObject* b)
{
    return 0;
}

void
js::NotifyGCPostSwap(JSObject* a, JSObject* b, unsigned removedFlags)
{
}

class GCSweepTask : public GCParallelTask
{
    GCSweepTask(const GCSweepTask&) = delete;

  public:
    explicit GCSweepTask(JSRuntime* rt) : GCParallelTask(rt) {}
    GCSweepTask(GCSweepTask&& other)
      : GCParallelTask(mozilla::Move(other))
    {}
};

// Causes the given WeakCache to be swept when run.
class SweepWeakCacheTask : public GCSweepTask
{
    JS::WeakCache<void*>& cache;

    SweepWeakCacheTask(const SweepWeakCacheTask&) = delete;

  public:
    SweepWeakCacheTask(JSRuntime* rt, JS::WeakCache<void*>& wc) : GCSweepTask(rt), cache(wc) {}
    SweepWeakCacheTask(SweepWeakCacheTask&& other)
      : GCSweepTask(mozilla::Move(other)), cache(other.cache)
    {}

    void run() override {
        cache.sweep();
    }
};

#define MAKE_GC_SWEEP_TASK(name)                                              \
    class name : public GCSweepTask {                                         \
        void run() override;                                                  \
      public:                                                                 \
        explicit name (JSRuntime* rt) : GCSweepTask(rt) {}                    \
    }
MAKE_GC_SWEEP_TASK(SweepAtomsTask);
MAKE_GC_SWEEP_TASK(SweepCCWrappersTask);
MAKE_GC_SWEEP_TASK(SweepBaseShapesTask);
MAKE_GC_SWEEP_TASK(SweepInitialShapesTask);
MAKE_GC_SWEEP_TASK(SweepObjectGroupsTask);
MAKE_GC_SWEEP_TASK(SweepRegExpsTask);
MAKE_GC_SWEEP_TASK(SweepMiscTask);
MAKE_GC_SWEEP_TASK(SweepCompressionTasksTask);
#undef MAKE_GC_SWEEP_TASK

/* virtual */ void
SweepAtomsTask::run()
{
}

/* virtual */ void
SweepCCWrappersTask::run()
{
}

/* virtual */ void
SweepObjectGroupsTask::run()
{
}

/* virtual */ void
SweepRegExpsTask::run()
{
}

/* virtual */ void
SweepMiscTask::run()
{
}

/* virtual */ void
SweepCompressionTasksTask::run()
{
#if 0 // OMRTODO: New sweeping code? Need to look at it
    AutoLockHelperThreadState lock;

    // Attach finished compression tasks.
    auto& finished = HelperThreadState().compressionFinishedList(lock);
    for (size_t i = 0; i < finished.length(); i++) {
        if (finished[i]->runtimeMatches(runtime())) {
            UniquePtr<SourceCompressionTask> task(Move(finished[i]));
            HelperThreadState().remove(finished, &i);
            task->complete();
        }
    }

    // Sweep pending tasks that are holding onto should-be-dead ScriptSources.
    auto& pending = HelperThreadState().compressionPendingList(lock);
    for (size_t i = 0; i < pending.length(); i++) {
        if (pending[i]->shouldCancel())
            HelperThreadState().remove(pending, &i);
    }
#endif
}

using WeakCacheTaskVector = mozilla::Vector<SweepWeakCacheTask, 0, SystemAllocPolicy>;

#if 0
// OMRTODO: New function?
/* static */ IncrementalProgress
GCRuntime::sweepTypeInformation(GCRuntime* gc, FreeOp* fop, Zone* zone, SliceBudget& budget,
                                AllocKind kind)
{
    return Finished;
}
#endif

// OMRTODO: New function
#if 0
/* static */ IncrementalProgress
GCRuntime::finalizeAllocKind(GCRuntime* gc, FreeOp* fop, Zone* zone, SliceBudget& budget,
                             AllocKind kind)
{
    // Set the number of things per arena for this AllocKind.
    size_t thingsPerArena = Arena::thingsPerArena(kind);
    auto& sweepList = gc->incrementalSweepList.ref();
    sweepList.setThingsPerArena(thingsPerArena);

    if (!zone->arenas.foregroundFinalize(fop, kind, budget, sweepList))
        return NotFinished;

    // Reset the slots of the sweep list that we used.
    sweepList.reset(thingsPerArena);
    return Finished;
}
#endif

// OMRTODO: New function
#if 0
/* static */ IncrementalProgress
GCRuntime::sweepShapeTree(GCRuntime* gc, FreeOp* fop, Zone* zone, SliceBudget& budget,
                          AllocKind kind)
{
    // Remove dead shapes from the shape tree, but don't finalize them yet.

    MOZ_ASSERT(kind == AllocKind::LIMIT);

    gcstats::AutoPhase ap(gc->stats(), gcstats::PHASE_SWEEP_SHAPE);

    ArenaLists& al = zone->arenas;

    if (!SweepArenaList<Shape>(&al.gcShapeArenasToUpdate.ref(), budget))
        return NotFinished;

    if (!SweepArenaList<AccessorShape>(&al.gcAccessorShapeArenasToUpdate.ref(), budget))
        return NotFinished;
    return Finished;
}
#endif

// OMRTODO: New function
static void
AddSweepPhase(bool* ok)
{
#if 0
    if (*ok)
        *ok = SweepPhases.emplaceBack();
#endif
}

// OMRTODO: New function
static void
AddSweepAction(bool* ok, SweepAction::Func func, AllocKind kind = AllocKind::LIMIT)
{
#if 0
    if (*ok)
        *ok = SweepPhases.back().emplaceBack(func, kind);
#endif
}

// OMRTODO: New function
/* static */ bool
GCRuntime::initializeSweepActions()
{
    bool ok = true;
#if 0
    AddSweepPhase(&ok);
    for (auto kind : ForegroundObjectFinalizePhase.kinds)
        AddSweepAction(&ok, GCRuntime::finalizeAllocKind, kind);

    AddSweepPhase(&ok);
    AddSweepAction(&ok, GCRuntime::sweepTypeInformation);
    AddSweepAction(&ok, GCRuntime::mergeSweptObjectArenas);

    for (const auto& finalizePhase : IncrementalFinalizePhases) {
        AddSweepPhase(&ok);
        for (auto kind : finalizePhase.kinds)
            AddSweepAction(&ok, GCRuntime::finalizeAllocKind, kind);
    }

    AddSweepPhase(&ok);
    AddSweepAction(&ok, GCRuntime::sweepShapeTree);
#endif
    return ok;
}

// OMRTODO: New function
#if 0
IncrementalProgress
GCRuntime::performSweepActions(SliceBudget& budget, AutoLockForExclusiveAccess& lock)
{
    AutoSetThreadIsSweeping threadIsSweeping;

    gcstats::AutoPhase ap(stats(), gcstats::PHASE_SWEEP);
    FreeOp fop(rt);

    if (drainMarkStack(budget, gcstats::PHASE_SWEEP_MARK) == NotFinished)
        return NotFinished;

    for (;;) {
        for (; sweepPhaseIndex < SweepPhases.length(); sweepPhaseIndex++) {
            const auto& actions = SweepPhases[sweepPhaseIndex];
            for (; sweepZone; sweepZone = sweepZone->nextNodeInGroup()) {
                for (; sweepActionIndex < actions.length(); sweepActionIndex++) {
                    const auto& action = actions[sweepActionIndex];
                    if (action.func(this, &fop, sweepZone, budget, action.kind) == NotFinished)
                        return NotFinished;
                }
                sweepActionIndex = 0;
            }
            sweepZone = currentSweepGroup;
        }
        sweepPhaseIndex = 0;

        endSweepingSweepGroup();
        getNextSweepGroup();
        if (!currentSweepGroup)
            return Finished;

        endMarkingSweepGroup();
        beginSweepingSweepGroup(lock);
    }
}
#endif

#ifdef DEBUG
static bool
AllNurseriesAreEmpty(JSRuntime* rt)
{
// OMRTODO: now there's multiple nurseries? look into this
#if 0
    for (ZoneGroupsIter group(rt); !group.done(); group.next()) {
        if (!group->nursery().isEmpty())
            return false;
    }
#endif
    return true;
}
#endif

JS_PUBLIC_API(JS::HeapState)
JS::CurrentThreadHeapState()
{
    return TlsContext.get()->heapState;
}

// OMRTODO: New function
bool
GCRuntime::canChangeActiveContext(JSContext* cx)
{
#if 0
    // Threads cannot be in the middle of any operation that affects GC
    // behavior when execution transfers to another thread for cooperative
    // scheduling.
    return cx->heapState == JS::HeapState::Idle
        && !cx->suppressGC
        && cx->allowGCBarriers
        && !cx->inUnsafeRegion
        && !cx->generationalDisabled
        && !cx->compactingDisabledCount
        && !cx->keepAtoms;
#endif
    return true;
}

namespace {

class AutoGCSlice {
  public:
    explicit AutoGCSlice(JSRuntime* rt);
    ~AutoGCSlice();
};

} /* anonymous namespace */

AutoGCSlice::AutoGCSlice(JSRuntime* rt)
{
}

AutoGCSlice::~AutoGCSlice()
{
}

namespace {

class AutoScheduleZonesForGC
{
  public:
    explicit AutoScheduleZonesForGC(JSRuntime* rt) {
    }

    ~AutoScheduleZonesForGC() {
    }
};

/*
 * An invariant of our GC/CC interaction is that there must not ever be any
 * black to gray edges in the system. It is possible to violate this with
 * simple compartmental GC. For example, in GC[n], we collect in both
 * compartmentA and compartmentB, and mark both sides of the cross-compartment
 * edge gray. Later in GC[n+1], we only collect compartmentA, but this time
 * mark it black. Now we are violating the invariants and must fix it somehow.
 *
 * To prevent this situation, we explicitly detect the black->gray state when
 * marking cross-compartment edges -- see ShouldMarkCrossCompartment -- adding
 * each violating edges to foundBlackGrayEdges. After we leave the trace
 * session for each GC slice, we "ExposeToActiveJS" on each of these edges
 * (which we cannot do safely from the guts of the GC).
 */
class AutoExposeLiveCrossZoneEdges
{
  public:
    explicit AutoExposeLiveCrossZoneEdges(BlackGrayEdgeVector* edgesPtr) {
    }
    ~AutoExposeLiveCrossZoneEdges() {
    }
};

} /* anonymous namespace */

#ifdef JS_GC_ZEAL
static bool
IsDeterministicGCReason(JS::gcreason::Reason reason)
{
    return true;
}
#endif

// OMRTODO: New function
#if 0
bool
GCRuntime::shouldRepeatForDeadZone(JS::gcreason::Reason reason)
{
    MOZ_ASSERT_IF(reason == JS::gcreason::COMPARTMENT_REVIVED, !isIncremental);

    if (!isIncremental || isIncrementalGCInProgress())
        return false;

    for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next()) {
        if (c->scheduledForDestruction)
            return true;
    }
    return false;
}
#endif

js::AutoEnqueuePendingParseTasksAfterGC::~AutoEnqueuePendingParseTasksAfterGC()
{
}

void
GCRuntime::notifyDidPaint()
{
}

void
GCRuntime::startDebugGC(JSGCInvocationKind gckind, SliceBudget& budget)
{
}

void
GCRuntime::debugGCSlice(SliceBudget& budget)
{
}

/* Schedule a full GC unless a zone will already be collected. */
void
js::PrepareForDebugGC(JSRuntime* rt)
{
}

void
GCRuntime::onOutOfMallocMemory()
{
}

void
GCRuntime::minorGC(JS::gcreason::Reason reason, gcstats::Phase phase)
{
}

JS::AutoDisableGenerationalGC::AutoDisableGenerationalGC(JSContext* cx)
  : cx(cx)
{
}

JS::AutoDisableGenerationalGC::~AutoDisableGenerationalGC()
{
}

JS_PUBLIC_API(bool)
JS::IsGenerationalGCEnabled(JSRuntime* rt)
{
    return false;
}

bool
GCRuntime::gcIfRequested()
{
    return false;
}

void
js::gc::FinishGC(JSContext* cx)
{
}

AutoPrepareForTracing::AutoPrepareForTracing(JSContext* cx, ZoneSelector selector)
{
    js::gc::FinishGC(cx);
    session_.emplace(cx->runtime());
}

JSCompartment*
js::NewCompartment(JSContext* cx, JSPrincipals* principals,
                   const JS::CompartmentOptions& options)
{
    JSRuntime* rt = cx->runtime();
    JS_AbortIfWrongThread(cx);

    ScopedJSDeletePtr<ZoneGroup> groupHolder;
    ScopedJSDeletePtr<Zone> zoneHolder;

    Zone* zone = nullptr;
    ZoneGroup* group = nullptr;
    JS::ZoneSpecifier zoneSpec = options.creationOptions().zoneSpecifier();
    switch (zoneSpec) {
      case JS::SystemZone:
        // systemZone and possibly systemZoneGroup might be null here, in which
        // case we'll make a zone/group and set these fields below.
        zone = rt->gc.systemZone;
        group = rt->gc.systemZoneGroup;
        break;
      case JS::ExistingZone:
        zone = static_cast<Zone*>(options.creationOptions().zonePointer());
        MOZ_ASSERT(zone);
        group = zone->group();
        break;
      case JS::NewZoneInNewZoneGroup:
        break;
      case JS::NewZoneInSystemZoneGroup:
        // As above, systemZoneGroup might be null here.
        group = rt->gc.systemZoneGroup;
        break;
      case JS::NewZoneInExistingZoneGroup:
        group = static_cast<ZoneGroup*>(options.creationOptions().zonePointer());
        MOZ_ASSERT(group);
        break;
    }

    if (group) {
        // Take over ownership of the group while we create the compartment/zone.
        group->enter();
    } else {
        MOZ_ASSERT(!zone);
        group = cx->new_<ZoneGroup>(rt);
        if (!group)
            return nullptr;

        groupHolder.reset(group);

        if (!group->init()) {
            ReportOutOfMemory(cx);
            return nullptr;
        }

        if (cx->generationalDisabled)
            group->nursery().disable();
    }

    if (!zone) {
#ifdef OMR
        // OMRTODO: Use multiple zones from a context correctly.
        OmrGcHelper::zone = zone;
#else
        zone = *rt->gc.zones.begin();
        zoneHolder.reset(zone);
#endif
    }

    ScopedJSDeletePtr<JSCompartment> compartment(cx->new_<JSCompartment>(zone, options));
    if (!compartment || !compartment->init(cx))
        return nullptr;

    // Set up the principals.
    JS_SetCompartmentPrincipals(compartment, principals);

    AutoLockGC lock(rt);

    if (!zone->compartments().append(compartment.get())) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    if (zoneHolder) {
        if (!group->zones().append(zone)) {
            ReportOutOfMemory(cx);
            return nullptr;
        }

        // Lazily set the runtime's sytem zone.
        if (zoneSpec == JS::SystemZone) {
            MOZ_RELEASE_ASSERT(!rt->gc.systemZone);
            rt->gc.systemZone = zone;
            zone->isSystem = true;
        }
    }

    if (groupHolder) {
        if (!rt->gc.groups.ref().append(group)) {
            ReportOutOfMemory(cx);
            return nullptr;
        }

        // Lazily set the runtime's system zone group.
        if (zoneSpec == JS::SystemZone || zoneSpec == JS::NewZoneInSystemZoneGroup) {
            MOZ_RELEASE_ASSERT(!rt->gc.systemZoneGroup);
            rt->gc.systemZoneGroup = group;
        }
    }

    zoneHolder.forget();
    groupHolder.forget();
    group->leave();
    return compartment.forget();
}

void
gc::MergeCompartments(JSCompartment* source, JSCompartment* target)
{
}

void
GCRuntime::setFullCompartmentChecks(bool enabled)
{
}

#ifdef JS_GC_ZEAL
bool
GCRuntime::selectForMarking(JSObject* object)
{
    return true;
}

void
GCRuntime::setDeterministic(bool enabled)
{
}
#endif

#ifdef DEBUG

/* Should only be called manually under gdb */
void PreventGCDuringInteractiveDebug()
{
}

#endif

void
js::ReleaseAllJITCode(FreeOp* fop)
{
}

AutoSuppressGC::AutoSuppressGC(JSContext* cx)
    : gc(cx->runtime()->gc)
{
    gc.disable();
}

AutoSuppressGC::~AutoSuppressGC()
{
    gc.enable();
}

#ifdef DEBUG
AutoDisableProxyCheck::AutoDisableProxyCheck()
{
}

AutoDisableProxyCheck::~AutoDisableProxyCheck()
{
}

JS_FRIEND_API(void)
JS::AssertGCThingMustBeTenured(JSObject* obj)
{
}

JS_FRIEND_API(void)
JS::AssertGCThingIsNotAnObjectSubclass(Cell* cell)
{
}

JS_FRIEND_API(void)
js::gc::AssertGCThingHasType(js::gc::Cell* cell, JS::TraceKind kind)
{
}
#endif

JS::AutoAssertNoGC::AutoAssertNoGC(JSContext* maybecx)
{
}

JS::AutoAssertNoGC::~AutoAssertNoGC()
{
}

JS::AutoAssertOnBarrier::AutoAssertOnBarrier(JSContext* cx)
{
}

JS::AutoAssertOnBarrier::~AutoAssertOnBarrier()
{
}

JS_FRIEND_API(bool)
js::gc::BarriersAreAllowedOnCurrentThread()
{
    return TlsContext.get()->allowGCBarriers;
}

#ifdef DEBUG
JS::AutoAssertNoAlloc::AutoAssertNoAlloc(JSContext* cx)
  : gc(nullptr)
{
}

void JS::AutoAssertNoAlloc::disallowAlloc(JSRuntime* rt)
{
}

JS::AutoAssertNoAlloc::~AutoAssertNoAlloc()
{
}

AutoAssertNoNurseryAlloc::AutoAssertNoNurseryAlloc()
{
}

AutoAssertNoNurseryAlloc::~AutoAssertNoNurseryAlloc()
{
}

JS::AutoEnterCycleCollection::AutoEnterCycleCollection(JSContext* cx)
{
}

JS::AutoEnterCycleCollection::~AutoEnterCycleCollection()
{
}

JS::AutoAssertGCCallback::AutoAssertGCCallback()
{
}
#endif

JS_FRIEND_API(const char*)
JS::GCTraceKindToAscii(JS::TraceKind kind)
{
    switch(kind) {
#define MAP_NAME(name, _0, _1) case JS::TraceKind::name: return #name;
JS_FOR_EACH_TRACEKIND(MAP_NAME);
#undef MAP_NAME
      default: return "Invalid";
    }
}

JS::GCCellPtr::GCCellPtr(const Value& v)
  : ptr(0)
{
    if (v.isString())
        ptr = checkedCast(v.toString(), JS::TraceKind::String);
    else if (v.isObject())
        ptr = checkedCast(&v.toObject(), JS::TraceKind::Object);
    else if (v.isSymbol())
        ptr = checkedCast(v.toSymbol(), JS::TraceKind::Symbol);
    else if (v.isPrivateGCThing())
        ptr = checkedCast(v.toGCThing(), v.toGCThing()->getTraceKind());
    else
        ptr = checkedCast(nullptr, JS::TraceKind::Null);
}

JS::TraceKind
JS::GCCellPtr::outOfLineKind() const
{
    return MapAllocToTraceKind(asCell()->asTenured().getAllocKind());
}

JS_PUBLIC_API(void)
JS::PrepareZoneForGC(Zone* zone)
{
}

JS_PUBLIC_API(void)
JS::PrepareForFullGC(JSContext* cx)
{
}

JS_PUBLIC_API(void)
JS::PrepareForIncrementalGC(JSContext* cx)
{
}

JS_PUBLIC_API(bool)
JS::IsGCScheduled(JSContext* cx)
{
    return false;
}

JS_PUBLIC_API(void)
JS::SkipZoneForGC(Zone* zone)
{
}

JS_PUBLIC_API(void)
JS::GCForReason(JSContext* cx, JSGCInvocationKind gckind, gcreason::Reason reason)
{
}

JS_PUBLIC_API(void)
JS::StartIncrementalGC(JSContext* cx, JSGCInvocationKind gckind, gcreason::Reason reason, int64_t millis)
{
}

JS_PUBLIC_API(void)
JS::IncrementalGCSlice(JSContext* cx, gcreason::Reason reason, int64_t millis)
{
}

JS_PUBLIC_API(void)
JS::FinishIncrementalGC(JSContext* cx, gcreason::Reason reason)
{
}

JS_PUBLIC_API(void)
JS::AbortIncrementalGC(JSContext* cx)
{
}

char16_t*
JS::GCDescription::formatSliceMessage(JSContext* cx) const
{
    return nullptr;
}

char16_t*
JS::GCDescription::formatSummaryMessage(JSContext* cx) const
{
    return nullptr;
}

JS::dbg::GarbageCollectionEvent::Ptr
JS::GCDescription::toGCEvent(JSContext* cx) const
{
    return JS::dbg::GarbageCollectionEvent::Create(cx->runtime(), cx->runtime()->gc.stats(),
                                                   cx->runtime()->gc.majorGCCount());
}

char16_t*
JS::GCDescription::formatJSON(JSContext* cx, uint64_t timestamp) const
{
    return nullptr;
}

JS_PUBLIC_API(JS::GCSliceCallback)
JS::SetGCSliceCallback(JSContext* cx, GCSliceCallback callback)
{
    return nullptr;
}

JS_PUBLIC_API(JS::DoCycleCollectionCallback)
JS::SetDoCycleCollectionCallback(JSContext* cx, JS::DoCycleCollectionCallback callback)
{
    return nullptr;
}

JS_PUBLIC_API(JS::GCNurseryCollectionCallback)
JS::SetGCNurseryCollectionCallback(JSContext* cx, GCNurseryCollectionCallback callback)
{
    return nullptr;
}

JS_PUBLIC_API(void)
JS::DisableIncrementalGC(JSContext* cx)
{
}

JS_PUBLIC_API(bool)
JS::IsIncrementalGCEnabled(JSContext* cx)
{
    return false;
}

JS_PUBLIC_API(bool)
JS::IsIncrementalGCInProgress(JSContext* cx)
{
    return false;
}

JS_PUBLIC_API(bool)
JS::IsIncrementalGCInProgress(JSRuntime* rt)
{
    return false;
}

JS_PUBLIC_API(bool)
JS::IsIncrementalBarrierNeeded(JSContext* cx)
{
    return false;
}

JS_PUBLIC_API(void)
JS::IncrementalPreWriteBarrier(JSObject* obj)
{
}

struct IncrementalReadBarrierFunctor {
    template <typename T> void operator()(T* t) { T::readBarrier(t); }
};

JS_PUBLIC_API(void)
JS::IncrementalReadBarrier(GCCellPtr thing)
{
}

JS_PUBLIC_API(bool)
JS::WasIncrementalGC(JSRuntime* rt)
{
    return false;
}

uint64_t
js::gc::NextCellUniqueId(JSRuntime* rt)
{
    return rt->gc.nextCellUniqueId();
}

namespace js {
namespace gc {
namespace MemInfo {

#ifdef JS_MORE_DETERMINISTIC
static bool
DummyGetter(JSContext* cx, unsigned argc, Value* vp)
{
    return true;
}
#endif

} /* namespace MemInfo */

JSObject*
NewMemoryInfoObject(JSContext* cx)
{
    RootedObject obj(cx, JS_NewObject(cx, nullptr));
    if (!obj)
        return nullptr;

    using namespace MemInfo;
    struct NamedGetter {
        const char* name;
        JSNative getter;
    } getters[] = {
        { "gcBytes", nullptr },
        { "gcMaxBytes", nullptr },
        { "mallocBytesRemaining", nullptr },
        { "maxMalloc", nullptr },
        { "gcIsHighFrequencyMode", nullptr },
        { "gcNumber", nullptr },
        { "majorGCCount", nullptr },
        { "minorGCCount", nullptr }
    };

    for (auto pair : getters) {
#ifdef JS_MORE_DETERMINISTIC
        JSNative getter = DummyGetter;
#else
        JSNative getter = pair.getter;
#endif
        if (!JS_DefineProperty(cx, obj, pair.name, UndefinedHandleValue,
                               JSPROP_ENUMERATE | JSPROP_SHARED,
                               getter, nullptr))
        {
            return nullptr;
        }
    }

    RootedObject zoneObj(cx, JS_NewObject(cx, nullptr));
    if (!zoneObj)
        return nullptr;

    if (!JS_DefineProperty(cx, obj, "zone", zoneObj, JSPROP_ENUMERATE))
        return nullptr;

    struct NamedZoneGetter {
        const char* name;
        JSNative getter;
    } zoneGetters[] = {
        { "gcBytes", nullptr },
        { "gcTriggerBytes", nullptr },
        { "gcAllocTrigger", nullptr },
        { "mallocBytesRemaining", nullptr },
        { "maxMalloc", nullptr },
        { "delayBytes", nullptr },
        { "heapGrowthFactor", nullptr },
        { "gcNumber", nullptr }
    };

    for (auto pair : zoneGetters) {
 #ifdef JS_MORE_DETERMINISTIC
        JSNative getter = DummyGetter;
#else
        JSNative getter = pair.getter;
#endif
        if (!JS_DefineProperty(cx, zoneObj, pair.name, UndefinedHandleValue,
                               JSPROP_ENUMERATE | JSPROP_SHARED,
                               getter, nullptr))
        {
            return nullptr;
        }
    }

    return obj;
}

const char*
StateName(State state)
{
    return "";
}

void
AutoAssertHeapBusy::checkCondition(JSRuntime *rt)
{
}

void
AutoAssertEmptyNursery::checkCondition(JSContext* cx) {
}

AutoEmptyNursery::AutoEmptyNursery(JSContext* cx)
{
}

} /* namespace gc */
} /* namespace js */

#ifdef DEBUG
void
js::gc::Cell::dump(FILE* fp) const
{
}

// For use in a debugger.
void
js::gc::Cell::dump() const
{
}
#endif

JS_PUBLIC_API(bool)
js::gc::detail::CellIsMarkedGrayIfKnown(const Cell* cell)
{
    return false;
}

#ifdef DEBUG
JS_PUBLIC_API(bool)
js::gc::detail::CellIsNotGray(const Cell* cell)
{
    return false;
}
#endif

