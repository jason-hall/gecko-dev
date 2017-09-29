/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Zone.h"

#include "jsgc.h"

#include "gc/Policy.h"
#include "jit/BaselineJIT.h"
#include "jit/Ion.h"
#include "jit/JitCompartment.h"
#include "vm/Debugger.h"
#include "vm/Runtime.h"

#include "jscompartmentinlines.h"
#include "jsgcinlines.h"

using namespace js;
using namespace js::gc;

JS::Zone::Zone(JSRuntime* rt, ZoneGroup* group)
  : JS::shadow::Zone(rt, &rt->gc.marker),
    group_(group),
    uniqueIds_(group),
    suppressAllocationMetadataBuilder(group, false),
    types(this),
    gcWeakMapList_(group),
    compartments_(),
    gcGrayRoots_(group),
    gcWeakRefs_(group),
    weakCaches_(group),
    gcWeakKeys_(group, SystemAllocPolicy(), rt->randomHashCodeScrambler()),
    gcSweepGroupEdges_(group),
    typeDescrObjects_(group, this),
    regExps(this),
    markedAtoms_(group),
    atomCache_(group),
    externalStringCache_(group),
    functionToStringCache_(group),
    gcDelayBytes(0),
    propertyTree_(group, this),
    baseShapes_(group, this),
    initialShapes_(group, this),
    nurseryShapes_(group),
    data(group, nullptr),
    isSystem(group, false),
#ifdef DEBUG
    gcLastSweepGroupIndex(group, 0),
#endif
    jitZone_(group, nullptr),
    gcScheduled_(false),
    gcPreserveCode_(group, false),
    keepShapeTables_(group, false)
{
    /* Ensure that there are no vtables to mess us up here. */
    MOZ_ASSERT(reinterpret_cast<JS::shadow::Zone*>(this) ==
               static_cast<JS::shadow::Zone*>(this));

    AutoLockGC lock(rt);
}

Zone::~Zone()
{
    JSRuntime* rt = runtimeFromAnyThread();
    if (this == rt->gc.systemZone)
        rt->gc.systemZone = nullptr;

    js_delete(jitZone_.ref());

#ifdef DEBUG
    // Avoid assertion destroying the weak map list if the embedding leaked GC things.
    if (!rt->gc.shutdownCollectedEverything())
        gcWeakMapList().clear();
#endif
}

bool
Zone::init(bool isSystemArg)
{
    isSystem = isSystemArg;
    return uniqueIds().init() &&
           gcSweepGroupEdges().init() &&
           gcWeakKeys().init() &&
           typeDescrObjects().init() &&
           markedAtoms().init() &&
           atomCache().init() &&
           regExps.init();
}

void
Zone::beginSweepTypes(FreeOp* fop, bool releaseTypes)
{
    AutoClearTypeInferenceStateOnOOM oom(this);
    types.beginSweep(fop, releaseTypes, oom);
}

Zone::DebuggerVector*
Zone::getOrCreateDebuggers(JSContext* cx)
{
    return nullptr;
}

void
Zone::sweepBreakpoints(FreeOp* fop)
{
    if (!group() || group()->debuggerList().isEmpty())
        return;

    /*
     * Sweep all compartments in a zone at the same time, since there is no way
     * to iterate over the scripts belonging to a single compartment in a zone.
     */

    MOZ_ASSERT(isGCSweepingOrCompacting());
    for (auto iter = cellIter<JSScript>(); !iter.done(); iter.next()) {
        JSScript* script = iter;
        if (!script->hasAnyBreakpointsOrStepMode())
            continue;

        bool scriptGone = IsAboutToBeFinalizedUnbarriered(&script);
        MOZ_ASSERT(script == iter);
        for (unsigned i = 0; i < script->length(); i++) {
            BreakpointSite* site = script->getBreakpointSite(script->offsetToPC(i));
            if (!site)
                continue;

            Breakpoint* nextbp;
            for (Breakpoint* bp = site->firstBreakpoint(); bp; bp = nextbp) {
                nextbp = bp->nextInSite();
                GCPtrNativeObject& dbgobj = bp->debugger->toJSObjectRef();

                // If we are sweeping, then we expect the script and the
                // debugger object to be swept in the same sweep group, except
                // if the breakpoint was added after we computed the sweep
                // groups. In this case both script and debugger object must be
                // live.
                MOZ_ASSERT_IF(isGCSweeping() && dbgobj->zone()->isCollecting(),
                              dbgobj->zone()->isGCSweeping() ||
                              (!scriptGone && dbgobj->asTenured().isMarkedAny()));

                bool dying = scriptGone || IsAboutToBeFinalized(&dbgobj);
                MOZ_ASSERT_IF(!dying, !IsAboutToBeFinalized(&bp->getHandlerRef()));
                if (dying)
                    bp->destroy(fop);
            }
        }
    }
}

void
Zone::sweepWeakMaps()
{
    /* Finalize unreachable (key,value) pairs in all weak maps. */
    WeakMapBase::sweepZone(this);
}

void
Zone::discardJitCode(FreeOp* fop, bool discardBaselineCode)
{
    if (!jitZone())
        return;

    if (isPreservingCode())
        return;

    if (discardBaselineCode) {
#ifdef DEBUG
        /* Assert no baseline scripts are marked as active. */
        for (auto script = cellIter<JSScript>(); !script.done(); script.next())
            MOZ_ASSERT_IF(script->hasBaselineScript(), !script->baselineScript()->active());
#endif

        /* Mark baseline scripts on the stack as active. */
        jit::MarkActiveBaselineScripts(this);
    }

    /* Only mark OSI points if code is being discarded. */
    jit::InvalidateAll(fop, this);

    for (auto script = cellIter<JSScript>(); !script.done(); script.next())  {
        jit::FinishInvalidation(fop, script);

        /*
         * Discard baseline script if it's not marked as active. Note that
         * this also resets the active flag.
         */
        if (discardBaselineCode)
            jit::FinishDiscardBaselineScript(fop, script);

        /*
         * Warm-up counter for scripts are reset on GC. After discarding code we
         * need to let it warm back up to get information such as which
         * opcodes are setting array holes or accessing getter properties.
         */
        script->resetWarmUpCounter();

        /*
         * Make it impossible to use the control flow graphs cached on the
         * BaselineScript. They get deleted.
         */
        if (script->hasBaselineScript())
            script->baselineScript()->setControlFlowGraph(nullptr);
    }

    /*
     * When scripts contains pointers to nursery things, the store buffer
     * can contain entries that point into the optimized stub space. Since
     * this method can be called outside the context of a GC, this situation
     * could result in us trying to mark invalid store buffer entries.
     *
     * Defer freeing any allocated blocks until after the next minor GC.
     */
    if (discardBaselineCode) {
        jitZone()->optimizedStubSpace()->freeAllAfterMinorGC(this);
        jitZone()->purgeIonCacheIRStubInfo();
    }

    /*
     * Free all control flow graphs that are cached on BaselineScripts.
     * Assuming this happens on the active thread and all control flow
     * graph reads happen on the active thread, this is safe.
     */
    jitZone()->cfgSpace()->lifoAlloc().freeAll();
}

js::jit::JitZone*
Zone::createJitZone(JSContext* cx)
{
    MOZ_ASSERT(!jitZone_);

    if (!cx->runtime()->getJitRuntime(cx))
        return nullptr;

    UniquePtr<jit::JitZone> jitZone(cx->new_<js::jit::JitZone>());
    if (!jitZone || !jitZone->init(cx))
        return nullptr;

    jitZone_ = jitZone.release();
    return jitZone_;
}

bool
Zone::hasMarkedCompartments()
{
    for (CompartmentsInZoneIter comp(this); !comp.done(); comp.next()) {
        if (comp->marked)
            return true;
    }
    return false;
}

void
Zone::notifyObservingDebuggers()
{
    for (CompartmentsInZoneIter comps(this); !comps.done(); comps.next()) {
        JSRuntime* rt = runtimeFromAnyThread();
        RootedGlobalObject global(TlsContext.get(), comps->unsafeUnbarrieredMaybeGlobal());
        if (!global)
            continue;

        GlobalObject::DebuggerVector* dbgs = global->getDebuggers();
        if (!dbgs)
            continue;

        for (GlobalObject::DebuggerVector::Range r = dbgs->all(); !r.empty(); r.popFront()) {
            if (!r.front()->debuggeeIsBeingCollected(rt->gc.majorGCCount())) {
#ifdef DEBUG
                fprintf(stderr,
                        "OOM while notifying observing Debuggers of a GC: The onGarbageCollection\n"
                        "hook will not be fired for this GC for some Debuggers!\n");
#endif
                return;
            }
        }
    }
}

void
Zone::clearTables()
{
    if (baseShapes().initialized())
        baseShapes().clear();
    if (initialShapes().initialized())
        initialShapes().clear();
}

bool
Zone::addTypeDescrObject(JSContext* cx, HandleObject obj)
{
    // Type descriptor objects are always tenured so we don't need post barriers
    // on the set.
    MOZ_ASSERT(!IsInsideNursery(obj));

    if (!typeDescrObjects().put(obj)) {
        ReportOutOfMemory(cx);
        return false;
    }

    return true;
}

JS_PUBLIC_API(void)
JS::shadow::RegisterWeakCache(JS::Zone* zone, detail::WeakCacheBase* cachep)
{
    zone->registerWeakCache(cachep);
}
