/*******************************************************************************
 * Copyright (c) 2017, 2017 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at http://eclipse.org/legal/epl-2.0
 * or the Apache License, Version 2.0 which accompanies this distribution
 * and is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following Secondary
 * Licenses when the conditions for such availability set forth in the
 * Eclipse Public License, v. 2.0 are satisfied: GNU General Public License,
 * version 2 with the GNU Classpath Exception [1] and GNU General Public
 * License, version 2 with the OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
 *******************************************************************************/

#include "omr.h"
#include "omrhashtable.h"

#include "EnvironmentBase.hpp"
#include "MarkingScheme.hpp"
#include "OMRVMThreadListIterator.hpp"
#include "Heap.hpp"
#include "HeapRegionIterator.hpp"
#include "ObjectHeapIteratorAddressOrderedList.hpp"
#include "OMRVMInterface.hpp"

#include "MarkingDelegate.hpp"

/// Spidermonkey Headers
#include "js/TracingAPI.h"

// JS
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
#include "jit/JitcodeMap.h"
#include "js/SliceBudget.h"
#include "vm/ArgumentsObject.h"
#include "vm/ArrayObject.h"
#include "vm/Debugger.h"
#include "vm/EnvironmentObject.h"
#include "vm/Scope.h"
#include "vm/Shape.h"
#include "vm/Stack.h"
#include "vm/Stack-inl.h"
#include "vm/Symbol.h"
#include "vm/TypedArrayObject.h"
#include "vm/UnboxedObject.h"
// #include "wasm/WasmJS.h"

#include "jscompartmentinlines.h"
#include "jsgcinlines.h"
#include "jsobjinlines.h"

// Spidermonkey Headers

#include "gc/Nursery-inl.h"
#include "vm/String-inl.h"
#include "vm/UnboxedObject-inl.h"

#include "gc/Barrier.h"
#include "gc/Marking.h"

#include "js/TracingAPI.h"
#include "js/GCPolicyAPI.h"

#include "jswatchpoint.h"

// OMR
#include "omrglue.hpp"

using namespace js;
using namespace JS;
using namespace js::gc;

namespace omrjs {

using JS::MapTypeToTraceKind;

using mozilla::ArrayLength;
using mozilla::DebugOnly;
using mozilla::IsBaseOf;
using mozilla::IsSame;
using mozilla::PodCopy;

OMRGCMarker::OMRGCMarker(JSRuntime* rt, MM_EnvironmentBase* env, MM_MarkingScheme* ms)
	: JSTracer(rt, JSTracer::TracerKindTag::OMR_SCAN, ExpandWeakMaps),
	  _env(env),
	  _markingScheme(ms) {
}

} // namespace omrjs

void
MM_MarkingDelegate::scanRoots(MM_EnvironmentBase *env)
{
	OMR_VM *omrVM = env->getOmrVM();
	JSRuntime *rt = (JSRuntime *)omrVM->_language_vm;

	// NOTE: The following code is from purgeRuntimes()
	gcstats::AutoPhase ap(rt->gc.stats(), gcstats::PhaseKind::PURGE);

	for (GCCompartmentsIter comp(rt); !comp.done(); comp.next())
		comp->purge();
	for (GCZonesIter zone(rt); !zone.done(); zone.next()) {
		zone->atomCache().clearAndShrink();
		zone->externalStringCache().purge();
		zone->functionToStringCache().purge();
	}
	for (const CooperatingContext& target : rt->cooperatingContexts()) {
		rt->gc.freeUnusedLifoBlocksAfterSweeping(&target.context()->tempLifoAlloc());
		target.context()->interpreterStack().purge(rt);
		target.context()->frontendCollectionPool().purge();
	}

	rt->caches().gsnCache.purge();
	rt->caches().envCoordinateNameCache.purge();
	rt->caches().newObjectCache.purge();
	rt->caches().uncompressedSourceCache.purge();
	if (rt->caches().evalCache.initialized())
		rt->caches().evalCache.clear();

	if (auto cache = rt->maybeThisRuntimeSharedImmutableStrings())
		cache->purge();
	//MOZ_ASSERT(rt->gc.unmarkGrayStack.empty());
	//rt->gc.unmarkGrayStack.clearAndFree();
	// End code from purgeRuntimes()

	if (NULL == _omrGCMarker) {
		MM_GCExtensionsBase *extensions = MM_GCExtensionsBase::getExtensions(omrVM);

		_omrGCMarker = (omrjs::OMRGCMarker *)extensions->getForge()->allocate(sizeof(omrjs::OMRGCMarker), MM_AllocationCategory::FIXED, OMR_GET_CALLSITE());
		new (_omrGCMarker) omrjs::OMRGCMarker(rt, env, _markingScheme);
	}

	gcstats::AutoPhase ap2(rt->gc.stats(), gcstats::PhaseKind::MARK_ROOTS);
	js::gc::AutoTraceSession session(rt);
	rt->gc.traceRuntimeAtoms(_omrGCMarker, session.lock);
	// JSCompartment::traceIncomingCrossCompartmentEdgesForZoneGC(trc);
	rt->gc.traceRuntimeCommon(_omrGCMarker, js::gc::GCRuntime::TraceOrMarkRuntime::TraceRuntime, session.lock);

	for (ZonesIter z(rt, WithAtoms); !z.done(); z.next()) {
		//Zone *zone = OmrGcHelper::zone;
		if (!z->gcWeakMapList().isEmpty()) {
			for (WeakMapBase* m : z->gcWeakMapList()) {
				m->trace(_omrGCMarker);
			}
		}
	}

	if (_omrGCMarker->runtime()->hasJitRuntime() && _omrGCMarker->runtime()->jitRuntime()->hasJitcodeGlobalTable()) {
		_omrGCMarker->runtime()->jitRuntime()->getJitcodeGlobalTable()->markIteratively(_omrGCMarker);
	}

	/* Mark Jitcode */
	/* Note: Original code used cell iter on trc->runtime()->atomsCompartment(lock)->zone(); */
	MM_HeapRegionManager *regionManager = env->getExtensions()->getHeap()->getHeapRegionManager();
	GC_HeapRegionIterator regionIterator(regionManager);

	MM_HeapRegionDescriptor *hrd = regionIterator.nextRegion();
	//AutoClearTypeInferenceStateOnOOM oom(zone);
	while (NULL != hrd) {
		GC_ObjectHeapIteratorAddressOrderedList objectIterator(env->getExtensions(), hrd, false);
		omrobjectptr_t omrobjPtr = objectIterator.nextObject();
		while (NULL != omrobjPtr) {
			js::gc::Cell *thing = (js::gc::Cell *)omrobjPtr;
			js::gc::AllocKind kind = thing->getAllocKind();
			if (kind == js::gc::AllocKind::JITCODE) {
				js::jit::JitCode* code = (js::jit::JitCode *)thing;
				TraceRoot(_omrGCMarker, &code, "wrapper");
			}
			omrobjPtr = objectIterator.nextObject();
		}
		hrd = regionIterator.nextRegion();
	}
}

void
MM_MarkingDelegate::masterCleanupAfterGC(MM_EnvironmentBase *env)
{
	OMR_VM *omrVM = env->getOmrVM();
	JSRuntime *rt = (JSRuntime *)omrVM->_language_vm;
	Zone *zone = OmrGcHelper::zone;
	js::AutoLockForExclusiveAccess lock(rt);

	/* Clear new object cache. Its entries may point to dead objects. */
	rt->activeContext()->caches().newObjectCache.clearNurseryObjects(rt);

	for (ZonesIter z(rt, WithAtoms); !z.done(); z.next()) {
		for (WeakMapBase* m : z->gcWeakMapList()) {
			m->sweep();
		}
		for (auto* cache : z->weakCaches()) {
			cache->sweep();
		}

		for (auto edge : z->gcWeakRefs()) {
			/* Edges may be present multiple times, so may already be nulled. */
			if (*edge && IsAboutToBeFinalizedDuringSweep(**edge)) {
				*edge = nullptr;
			}
		}
		z->gcWeakRefs().clear();

		/* No need to look up any more weakmap keys from this zone group. */
		AutoEnterOOMUnsafeRegion oomUnsafe;
		if (!z->gcWeakKeys().clear()) {
			oomUnsafe.crash("clearing weak keys in beginSweepingZoneGroup()");
		}
	}
	for (JS::detail::WeakCacheBase* cache : rt->weakCaches()) {
		cache->sweep();
	}

	FreeOp fop(rt);

	// callFinalizeCallbacks(&fop, JSFINALIZE_GROUP_START);
    // callWeakPointerZoneGroupCallbacks();

    // for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next())
    //         callWeakPointerCompartmentCallbacks(comp);
    // }

	// Cancel any active or pending off thread compilations.
	js::CancelOffThreadIonCompile(rt, JS::Zone::Sweep);

	for (GCCompartmentsIter c(rt); !c.done(); c.next()) {
		c->sweepTemplateLiteralMap();
		c->sweepVarNames();
		c->sweepGlobalObject();
		c->sweepDebugEnvironments();
		c->sweepJitCompartment(&fop);
		c->sweepTemplateObjects();
	}

	// Bug 1071218: the following two methods have not yet been
	// refactored to work on a single zone-group at once.

	// Collect watch points associated with unreachable objects.
	WatchpointMap::sweepAll(rt);

	// Detach unreachable debuggers and global objects from each other.
	Debugger::sweepAll(&fop);

	// Sweep entries containing about-to-be-finalized JitCode and
	// update relocated TypeSet::Types inside the JitcodeGlobalTable.
	jit::JitRuntime::SweepJitcodeGlobalTable(rt);

	for (ZonesIter z(rt, WithAtoms); !z.done(); z.next()) {
		if (jit::JitZone* jitZone = z->jitZone())
			jitZone->sweep(&fop);
	}

	for (ZonesIter z(rt, WithAtoms); !z.done(); z.next()) {
		z->discardJitCode(&fop);
	}

	for (ZonesIter z(rt, WithAtoms); !z.done(); z.next()) {
		z->beginSweepTypes(&fop, !zone->isPreservingCode());
		z->sweepBreakpoints(&fop);
		z->sweepUniqueIds(&fop);
	}

	rt->symbolRegistry(lock).sweep();

	// Sweep atoms
	rt->atomsForSweeping()->sweep();

	for (GCCompartmentsIter c(rt); !c.done(); c.next()) {
		c->sweepCrossCompartmentWrappers();
	}

	for (GCCompartmentsIter c(rt); !c.done(); c.next())
		c->sweepRegExps();

	for (GCCompartmentsIter c(rt); !c.done(); c.next())
		c->objectGroups.sweep(rt->defaultFreeOp());

	for (GCCompartmentsIter c(rt); !c.done(); c.next()) {
		c->sweepSavedStacks();
		c->sweepSelfHostingScriptSource();
		c->sweepNativeIterators();
	}

	// NOTE: This wasn't in the original sweep code, but stopped a crash on using a freed object from iteratorCache
	for (GCCompartmentsIter c(rt); !c.done(); c.next())
		c->purge();

	rt->gc.callFinalizeCallbacks(&fop, JSFINALIZE_GROUP_END);

	zone->types.endSweep(rt);

	/* This puts the heap into the state required to walk it */
	GC_OMRVMInterface::flushCachesForGC(env);

	MM_HeapRegionManager *regionManager = env->getExtensions()->getHeap()->getHeapRegionManager();
	{
		GC_HeapRegionIterator regionIterator(regionManager);

		/* Walk the heap for sweeping. */
		MM_HeapRegionDescriptor *hrd = regionIterator.nextRegion();
		AutoClearTypeInferenceStateOnOOM oom(zone);
		while (NULL != hrd) {
			GC_ObjectHeapIteratorAddressOrderedList objectIterator(env->getExtensions(), hrd, false);
			omrobjectptr_t omrobjPtr = objectIterator.nextObject();
			while (NULL != omrobjPtr) {
				/* Sweep scripts, object groups, and shapes. */
				js::gc::Cell *thing = (js::gc::Cell *)omrobjPtr;
				js::gc::AllocKind kind = thing->getAllocKind();
				if (kind == js::gc::AllocKind::SHAPE || kind == js::gc::AllocKind::ACCESSOR_SHAPE /*|| kind == js::gc::AllocKind::BASE_SHAPE*/) {
					if (!((Shape *)thing)->isMarkedAny()) {
						((Shape *)thing)->sweep();
					}
				} else if (kind == js::gc::AllocKind::OBJECT_GROUP) {
					((ObjectGroup *)thing)->maybeSweep(&oom);
				} else if (kind == js::gc::AllocKind::SCRIPT /*|| kind == js::gc::AllocKind::LAZY_SCRIPT*/) {
					((JSScript *)thing)->maybeSweepTypes(&oom);
				} else if (((int)kind) >= (int)js::gc::AllocKind::OBJECT0 && ((int)kind) <= (int)js::gc::AllocKind::OBJECT16_BACKGROUND) {
					JSObject *obj = (JSObject *)thing;
					if (obj->is<js::NativeObject>() && !_markingScheme->isMarked(omrobjPtr)) {
						obj->as<js::NativeObject>().deleteAllSlots();
					}
				}
				omrobjPtr = objectIterator.nextObject();
			}
			hrd = regionIterator.nextRegion();
		}
	}
	// Finalize unmarked objects.
	{
		GC_HeapRegionIterator regionIterator(regionManager);

		MM_HeapRegionDescriptor *hrd = regionIterator.nextRegion();
		AutoClearTypeInferenceStateOnOOM oom(zone);
		while (NULL != hrd) {
			GC_ObjectHeapIteratorAddressOrderedList objectIterator(env->getExtensions(), hrd, false);
			omrobjectptr_t omrobjPtr = objectIterator.nextObject();
			while (NULL != omrobjPtr) {
				if (!_markingScheme->isMarked(omrobjPtr)) {
					js::gc::Cell *thing = (js::gc::Cell *)omrobjPtr;
					js::gc::AllocKind kind = thing->getAllocKind();
					if ((int)js::gc::AllocKind::OBJECT_LAST > (int)kind) {
						((JSObject *)thing)->finalize(&fop);
					} else if (js::gc::AllocKind::OBJECT_LIMIT == kind) {
						((JSScript *)thing)->finalize(&fop);
					} else if (js::gc::AllocKind::LAZY_SCRIPT == kind) {
						((js::LazyScript *)thing)->finalize(&fop);
					} else if (js::gc::AllocKind::JITCODE == kind) {
						((js::jit::JitCode *)thing)->finalize(&fop);
					}
				}
				omrobjPtr = objectIterator.nextObject();
			}
			hrd = regionIterator.nextRegion();
		}
	}
	{
		GC_HeapRegionIterator regionIterator(regionManager);

		/* Walk the heap, for objects that are not marked we corrupt them to maximize the chance we will crash immediately
		if they are used. For live objects validate that they have the expected eyecatcher */
		MM_HeapRegionDescriptor *hrd = regionIterator.nextRegion();
		AutoClearTypeInferenceStateOnOOM oom(zone);
		while (NULL != hrd) {
			/* Walk all of the objects, making sure that those that were not marked are no longer
			usable. If they are later used we will know this and optimally crash */
			GC_ObjectHeapIteratorAddressOrderedList objectIterator(env->getExtensions(), hrd, false);
			omrobjectptr_t omrobjPtr = objectIterator.nextObject();
			while (NULL != omrobjPtr) {
				if (!_markingScheme->isMarked(omrobjPtr)) {
					/* object will be collected. We write the full contents of the object with a known value. */
					uintptr_t objsize = env->getExtensions()->objectModel.getConsumedSizeInBytesWithHeader(omrobjPtr);
					memset(omrobjPtr, 0x5E, (size_t)objsize);
					MM_HeapLinkedFreeHeader::fillWithHoles(omrobjPtr, objsize);
				}
				omrobjPtr = objectIterator.nextObject();
			}
			hrd = regionIterator.nextRegion();
		}
	}
	rt->gc.incGcNumber();
}
