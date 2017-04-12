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

JS::Zone::Zone(JSRuntime* rt)
  : JS::shadow::Zone(rt, nullptr),
    suppressAllocationMetadataBuilder(false),
#ifndef OMR
    arenas(rt),
#endif
    types(this),
	gcWeakMapList(),
    compartments(),
    gcGrayRoots(),
    typeDescrObjects(this, SystemAllocPolicy()),
    gcMallocBytes(0),
    gcMallocGCTriggered(false),
    usage(&rt->gc.usage),
    gcDelayBytes(0),
    propertyTree(this),
    baseShapes(this, BaseShapeSet()),
    initialShapes(this, InitialShapeSet()),
    data(nullptr),
    isSystem(false),
    usedByExclusiveThread(false),
    active(false),
    jitZone_(nullptr) {}

Zone::DebuggerVector*
Zone::getOrCreateDebuggers(JSContext* cx)
{
    return nullptr;
}

Zone::~Zone()
{
    js_delete(jitZone_);
}

MOZ_MUST_USE bool
Zone::init(bool isSystem) { 
	return uniqueIds_.init() &&
	    gcWeakKeys.init() &&
		gcZoneGroupEdges.init() &&
		typeDescrObjects.init();
}

JS_PUBLIC_API(void)
JS::shadow::RegisterWeakCache(JS::Zone* zone, WeakCache<void*>* cachep)
{
    zone->registerWeakCache(cachep);
}

js::jit::JitZone*
Zone::createJitZone(JSContext* cx)
{
    MOZ_ASSERT(!jitZone_);

    if (!cx->runtime()->getJitRuntime(cx))
        return nullptr;

    jitZone_ = cx->new_<js::jit::JitZone>();
    return jitZone_;
}

void
Zone::clearTables()
{
    if (baseShapes.initialized())
        baseShapes.clear();
    if (initialShapes.initialized())
        initialShapes.clear();
}

void
Zone::beginSweepTypes(FreeOp* fop, bool releaseTypes)
{
    // Periodically release observed types for all scripts. This is safe to
    // do when there are no frames for the zone on the stack.
    if (active)
        releaseTypes = false;

    AutoClearTypeInferenceStateOnOOM oom(this);
    types.beginSweep(fop, releaseTypes, oom);
}

void
Zone::sweepBreakpoints(FreeOp* fop)
{
    if (fop->runtime()->debuggerList.isEmpty())
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
                // debugger object to be swept in the same zone group, except if
                // the breakpoint was added after we computed the zone
                // groups. In this case both script and debugger object must be
                // live.
                MOZ_ASSERT_IF(isGCSweeping() && dbgobj->zone()->isCollecting(),
                              dbgobj->zone()->isGCSweeping() ||
                              (!scriptGone && dbgobj->asTenured().isMarked()));

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
Zone::discardJitCode(FreeOp* fop)
{
    if (!jitZone())
        return;

    if (isPreservingCode()) {
        PurgeJITCaches(this);
    } else {

#ifdef DEBUG
        /* Assert no baseline scripts are marked as active. */
        for (auto script = cellIter<JSScript>(); !script.done(); script.next())
            MOZ_ASSERT_IF(script->hasBaselineScript(), !script->baselineScript()->active());
#endif

        /* Mark baseline scripts on the stack as active. */
        jit::MarkActiveBaselineScripts(this);

        /* Only mark OSI points if code is being discarded. */
        jit::InvalidateAll(fop, this);

        for (auto script = cellIter<JSScript>(); !script.done(); script.next())  {
            jit::FinishInvalidation(fop, script);

            /*
             * Discard baseline script if it's not marked as active. Note that
             * this also resets the active flag.
             */
            jit::FinishDiscardBaselineScript(fop, script);

            /*
             * Warm-up counter for scripts are reset on GC. After discarding code we
             * need to let it warm back up to get information such as which
             * opcodes are setting array holes or accessing getter properties.
             */
            script->resetWarmUpCounter();
        }

        /*
         * When scripts contains pointers to nursery things, the store buffer
         * can contain entries that point into the optimized stub space. Since
         * this method can be called outside the context of a GC, this situation
         * could result in us trying to mark invalid store buffer entries.
         *
         * Defer freeing any allocated blocks until after the next minor GC.
         */
        jitZone()->optimizedStubSpace()->freeAllAfterMinorGC(fop->runtime());
    }
}
