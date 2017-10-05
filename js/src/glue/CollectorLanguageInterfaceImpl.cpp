/*******************************************************************************
 *
 * (c) Copyright IBM Corp. 1991, 2016
 *
 *  This program and the accompanying materials are made available
 *  under the terms of the Eclipse Public License v1.0 and
 *  Apache License v2.0 which accompanies this distribution.
 *
 *      The Eclipse Public License is available at
 *      http://www.eclipse.org/legal/epl-v10.html
 *
 *      The Apache License v2.0 is available at
 *      http://www.opensource.org/licenses/apache2.0.php
 *
 * Contributors:
 *    Multiple authors (IBM Corp.) - initial implementation and documentation
 *******************************************************************************/

#include "GeckoConfig.hpp" // TODO: This is a total hack

#include "modronbase.h"

#include "CollectorLanguageInterfaceImpl.hpp"
#if defined(OMR_GC_MODRON_CONCURRENT_MARK)
#include "ConcurrentSafepointCallback.hpp"
#endif /* OMR_GC_MODRON_CONCURRENT_MARK */
#if defined(OMR_GC_MODRON_COMPACTION)
#include "CompactScheme.hpp"
#endif /* OMR_GC_MODRON_COMPACTION */
#include "EnvironmentStandard.hpp"
#include "ForwardedHeader.hpp"
#include "GCExtensionsBase.hpp"
#include "HeapLinkedFreeHeader.hpp"
#include "MarkingScheme.hpp"
#include "mminitcore.h"
#include "objectdescription.h"
#include "ObjectModel.hpp"
#include "omr.h"
#include "omrvm.h"
#include "OMRVMInterface.hpp"
#include "Scavenger.hpp"
#include "SlotObject.hpp"

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
#include "js/SliceBudget.h"
#include "vm/ArgumentsObject.h"
#include "vm/ArrayObject.h"
#include "vm/Debugger.h"
#include "vm/EnvironmentObject.h"
#include "vm/Scope.h"
#include "vm/Shape.h"
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

#include "CollectorLanguageInterfaceImpl.hpp"
#include "Dispatcher.hpp"
#include "Heap.hpp"
#include "HeapRegionIterator.hpp"
#include "MarkingScheme.hpp"
#include "ObjectHeapIteratorAddressOrderedList.hpp"
#include "ObjectMap.hpp"
#include "OMRVMInterface.hpp"
#include "Task.hpp"

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

/* This enum extends ConcurrentStatus with values > CONCURRENT_ROOT_TRACING. Values from this
 * and from ConcurrentStatus are treated as uintptr_t values everywhere except when used as
 * case labels in switch() statements where manifest constants are required.
 * 
 * ConcurrentStatus extensions allow the client language to define discrete units of work
 * that can be executed in parallel by concurrent threads. ConcurrentGC will call 
 * MM_CollectorLanguageInterfaceImpl::concurrentGC_collectRoots(..., concurrentStatus, ...)
 * only once with each client-defined status value. The thread that receives the call
 * can check the concurrentStatus value to select and execute the appropriate unit of work.
 */
enum {
	CONCURRENT_ROOT_TRACING1 = ((uintptr_t)((uintptr_t)CONCURRENT_ROOT_TRACING + 1))
};

/**
 * Initialization
 */
MM_CollectorLanguageInterfaceImpl *
MM_CollectorLanguageInterfaceImpl::newInstance(MM_EnvironmentBase *env)
{
	MM_CollectorLanguageInterfaceImpl *cli = NULL;
	OMR_VM *omrVM = env->getOmrVM();
	MM_GCExtensionsBase *extensions = MM_GCExtensionsBase::getExtensions(omrVM);

	cli = (MM_CollectorLanguageInterfaceImpl *)extensions->getForge()->allocate(sizeof(MM_CollectorLanguageInterfaceImpl), MM_AllocationCategory::FIXED, OMR_GET_CALLSITE());
	if (NULL != cli) {
		new(cli) MM_CollectorLanguageInterfaceImpl(omrVM);
		if (!cli->initialize(omrVM)) {
			cli->kill(env);
			cli = NULL;
		}
	}

	return cli;
}

void
MM_CollectorLanguageInterfaceImpl::kill(MM_EnvironmentBase *env)
{
	OMR_VM *omrVM = env->getOmrVM();
	tearDown(omrVM);
	MM_GCExtensionsBase::getExtensions(omrVM)->getForge()->free(this);
}

void
MM_CollectorLanguageInterfaceImpl::tearDown(OMR_VM *omrVM)
{

}

bool
MM_CollectorLanguageInterfaceImpl::initialize(OMR_VM *omrVM)
{
	return true;
}

void
MM_CollectorLanguageInterfaceImpl::flushNonAllocationCaches(MM_EnvironmentBase *env)
{
}

OMR_VMThread *
MM_CollectorLanguageInterfaceImpl::attachVMThread(OMR_VM *omrVM, const char *threadName, uintptr_t reason)
{
	OMR_VMThread *omrVMThread = NULL;
	omr_error_t rc = OMR_ERROR_NONE;

	rc = OMR_Glue_BindCurrentThread(omrVM, threadName, &omrVMThread);
	if (OMR_ERROR_NONE != rc) {
		return NULL;
	}
	return omrVMThread;
}

void
MM_CollectorLanguageInterfaceImpl::detachVMThread(OMR_VM *omrVM, OMR_VMThread *omrVMThread, uintptr_t reason)
{
	if (NULL != omrVMThread) {
		OMR_Glue_UnbindCurrentThread(omrVMThread);
	}
}

void
MM_CollectorLanguageInterfaceImpl::markingScheme_masterSetupForGC(MM_EnvironmentBase *env)
{
	Zone *zone = OmrGcHelper::zone;
	zone->runtimeFromActiveCooperatingThread()->gc.incGcNumber();
}

void
MM_CollectorLanguageInterfaceImpl::markingScheme_scanRoots(MM_EnvironmentBase *env)
{
	if (J9MODRON_HANDLE_NEXT_WORK_UNIT(env)) {
		OMR_VM *omrVM = env->getOmrVM();
		JSRuntime *rt = (JSRuntime *)omrVM->_language_vm;
		if (NULL == _omrGCMarker) {
			MM_GCExtensionsBase *extensions = MM_GCExtensionsBase::getExtensions(omrVM);

			_omrGCMarker = (omrjs::OMRGCMarker *)extensions->getForge()->allocate(sizeof(omrjs::OMRGCMarker), MM_AllocationCategory::FIXED, OMR_GET_CALLSITE());
			new (_omrGCMarker) omrjs::OMRGCMarker(rt, env, _markingScheme);
		}

		gcstats::AutoPhase ap(rt->gc.stats(), gcstats::PhaseKind::MARK_ROOTS);
		js::gc::AutoTraceSession session(rt);
		rt->gc.traceRuntimeAtoms(_omrGCMarker, session.lock);
		// JSCompartment::traceIncomingCrossCompartmentEdgesForZoneGC(trc);
		rt->gc.traceRuntimeCommon(_omrGCMarker, js::gc::GCRuntime::TraceOrMarkRuntime::TraceRuntime, session.lock);

		//for (GCZoneGroupIter zone(rt); !zone.done(); zone.next()) {
		Zone *zone = OmrGcHelper::zone;
		if (!zone->gcWeakMapList().isEmpty()) {
			for (WeakMapBase* m : zone->gcWeakMapList()) {
				m->trace(_omrGCMarker);
			}
		}
	}
}

void
MM_CollectorLanguageInterfaceImpl::markingScheme_completeMarking(MM_EnvironmentBase *env)
{
}

void
MM_CollectorLanguageInterfaceImpl::markingScheme_markLiveObjectsComplete(MM_EnvironmentBase *env)
{
}

void
MM_CollectorLanguageInterfaceImpl::markingScheme_masterSetupForWalk(MM_EnvironmentBase *env)
{
}

void
MM_CollectorLanguageInterfaceImpl::markingScheme_masterCleanupAfterGC(MM_EnvironmentBase *env)
{
}

struct TraceChildrenFunctor {
	MM_CollectorLanguageInterfaceImpl *cli;

	template <typename T>
	void operator()(T* thing) {
		static_cast<T*>(thing)->traceChildren(cli->_omrGCMarker);
	}
};


uintptr_t
MM_CollectorLanguageInterfaceImpl::markingScheme_scanObject(MM_EnvironmentBase *env, omrobjectptr_t objectPtr, MarkingSchemeScanReason reason)
{
	TraceChildrenFunctor traceChildren = {this};
	if (JS::TraceKind::Null != ((Cell *)objectPtr)->getTraceKind()) {
		DispatchTraceKindTyped(traceChildren, (Cell *)objectPtr, ((Cell *)objectPtr)->getTraceKind());
	}
	return 0;
}

#if defined(OMR_GC_MODRON_CONCURRENT_MARK)
uintptr_t
MM_CollectorLanguageInterfaceImpl::markingScheme_scanObjectWithSize(MM_EnvironmentBase *env, omrobjectptr_t objectPtr, MarkingSchemeScanReason reason, uintptr_t sizeToDo)
{
#error implement an object scanner which scans up to sizeToDo bytes
}
#endif /* OMR_GC_MODRON_CONCURRENT_MARK */

void
MM_CollectorLanguageInterfaceImpl::parallelDispatcher_handleMasterThread(OMR_VMThread *omrVMThread)
{
	/* Do nothing for now.  only required for SRT */
}

#if defined(OMR_GC_MODRON_SCAVENGER)
void
MM_CollectorLanguageInterfaceImpl::scavenger_reportObjectEvents(MM_EnvironmentBase *env)
{
	/* Do nothing for now */
}

void
MM_CollectorLanguageInterfaceImpl::scavenger_masterSetupForGC(MM_EnvironmentBase *env)
{
	/* Do nothing for now */
}

void
MM_CollectorLanguageInterfaceImpl::scavenger_workerSetupForGC_clearEnvironmentLangStats(MM_EnvironmentBase *env)
{
	/* Do nothing for now */
}

void
MM_CollectorLanguageInterfaceImpl::scavenger_reportScavengeEnd(MM_EnvironmentBase * envBase, bool scavengeSuccessful)
{
	/* Do nothing for now */
}

void
MM_CollectorLanguageInterfaceImpl::scavenger_mergeGCStats_mergeLangStats(MM_EnvironmentBase *envBase)
{
	/* Do nothing for now */
}

void
MM_CollectorLanguageInterfaceImpl::scavenger_masterThreadGarbageCollect_scavengeComplete(MM_EnvironmentBase *envBase)
{
	/* Do nothing for now */
}

void
MM_CollectorLanguageInterfaceImpl::scavenger_masterThreadGarbageCollect_scavengeSuccess(MM_EnvironmentBase *envBase)
{
	/* Do nothing for now */
}

bool
MM_CollectorLanguageInterfaceImpl::scavenger_internalGarbageCollect_shouldPercolateGarbageCollect(MM_EnvironmentBase *envBase, PercolateReason *reason, uint32_t *gcCode)
{
#error Return true if scavenge cycle should be forgone and GC cycle percolated up to another collector
}

GC_ObjectScanner *
MM_CollectorLanguageInterfaceImpl::scavenger_getObjectScanner(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr, void *allocSpace, uintptr_t flags)
{
#error Implement a GC_ObjectScanner subclass for each distinct kind of objects (eg scanner for scalar objects and scanner for indexable objects)
}

void
MM_CollectorLanguageInterfaceImpl::scavenger_flushReferenceObjects(MM_EnvironmentStandard *env)
{
	/* Do nothing for now */
}

bool
MM_CollectorLanguageInterfaceImpl::scavenger_hasIndirectReferentsInNewSpace(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr)
{
	/* This method must be implemented and return true an object may hold any object references that are live
	 * but not reachable by traversing the reference graph from the root set or remembered set. Otherwise this
	 * default implementation should be used.
	 */
	return false;
}

bool
MM_CollectorLanguageInterfaceImpl::scavenger_scavengeIndirectObjectSlots(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr)
{
	/* This method must be implemented if an object may hold any object references that are live but not reachable
	 * by traversing the reference graph from the root set or remembered set. In that case, this method should
	 * call MM_Scavenger::copyObjectSlot(..) for each such indirect object reference, ORing the boolean result
	 * from each call to MM_Scavenger::copyObjectSlot(..) into a single boolean value to be returned.
	 */
	return false;
}

void
MM_CollectorLanguageInterfaceImpl::scavenger_backOutIndirectObjectSlots(MM_EnvironmentStandard *env, omrobjectptr_t objectPtr)
{
	/* This method must be implemented if an object may hold any object references that are live but not reachable
	 * by traversing the reference graph from the root set or remembered set. In that case, this method should
	 * call MM_Scavenger::backOutFixSlotWithoutCompression(..) for each uncompressed slot holding a reference to
	 * an indirect object that is associated with the object.
	 */
}

void
MM_CollectorLanguageInterfaceImpl::scavenger_backOutIndirectObjects(MM_EnvironmentStandard *env)
{
	/* This method must be implemented if an object may hold any object references that are live but not reachable
	 * by traversing the reference graph from the root set or remembered set. In that case, this method should locate
	 * all such objects and call MM_Scavenger::backOutObjectScan(..) for each such object that is in the remembered
	 * set. For example,
	 *
	 * if (_extensions->objectModel.isRemembered(indirectObject)) {
	 *    _extensions->scavenger->backOutObjectScan(env, indirectObject);
	 * }
	 */
}

void
MM_CollectorLanguageInterfaceImpl::scavenger_reverseForwardedObject(MM_EnvironmentBase *env, MM_ForwardedHeader *forwardedHeader)
{
	/* This method must restore the object header slot (and overlapped slot, if header is compressed)
	 * in the original object and install a reverse forwarded object in the forwarding location. 
	 * A reverse forwarded object is a hole (MM_HeapLinkedFreeHeader) whose 'next' pointer actually 
	 * points at the original object. This keeps tenure space walkable once the reverse forwarded 
	 * objects are abandoned.
	 */
}

#if defined (OMR_INTERP_COMPRESSED_OBJECT_HEADER)
void
MM_CollectorLanguageInterfaceImpl::scavenger_fixupDestroyedSlot(MM_EnvironmentBase *env, MM_ForwardedHeader *forwardedHeader, MM_MemorySubSpaceSemiSpace *subSpaceNew)
{
	/* This method must be implemented if (and only if) the object header is stored in a compressed slot. in that
	 * case the other half of the full (omrobjectptr_t sized) slot may hold a compressed object reference that
	 * must be restored by this method.
	 */
	Assert_MM_unimplemented();
}
#endif /* OMR_INTERP_COMPRESSED_OBJECT_HEADER */
#endif /* OMR_GC_MODRON_SCAVENGER */

#if defined(OMR_GC_MODRON_COMPACTION)
void
MM_CollectorLanguageInterfaceImpl::compactScheme_verifyHeap(MM_EnvironmentBase *env, MM_MarkMap *markMap)
{
	Assert_MM_unimplemented();
}

void
MM_CollectorLanguageInterfaceImpl::compactScheme_fixupRoots(MM_EnvironmentBase *env, MM_CompactScheme *compactScheme)
{
	Assert_MM_unimplemented();
}

void
MM_CollectorLanguageInterfaceImpl::compactScheme_workerCleanupAfterGC(MM_EnvironmentBase *env)
{
	Assert_MM_unimplemented();
}

void
MM_CollectorLanguageInterfaceImpl::compactScheme_languageMasterSetupForGC(MM_EnvironmentBase *env)
{
	Assert_MM_unimplemented();
}
#endif /* OMR_GC_MODRON_COMPACTION */

#if defined(OMR_GC_MODRON_CONCURRENT_MARK)
MM_ConcurrentSafepointCallback*
MM_CollectorLanguageInterfaceImpl::concurrentGC_createSafepointCallback(MM_EnvironmentBase *env)
{
	MM_EnvironmentStandard *envStd = MM_EnvironmentStandard::getEnvironment(env);
	return MM_ConcurrentSafepointCallback::newInstance(envStd);
}

uintptr_t
MM_CollectorLanguageInterfaceImpl::concurrentGC_getNextTracingMode(uintptr_t executionMode)
{
	uintptr_t nextExecutionMode = CONCURRENT_TRACE_ONLY;
	switch (executionMode) {
	case CONCURRENT_ROOT_TRACING:
		nextExecutionMode = CONCURRENT_ROOT_TRACING1;
		break;
	case CONCURRENT_ROOT_TRACING1:
		nextExecutionMode = CONCURRENT_TRACE_ONLY;
		break;
	default:
		Assert_MM_unreachable();
	}

	return nextExecutionMode;
}

uintptr_t
MM_CollectorLanguageInterfaceImpl::concurrentGC_collectRoots(MM_EnvironmentStandard *env, uintptr_t concurrentStatus, bool *collectedRoots, bool *paidTax)
{
	uintptr_t bytesScanned = 0;
	*collectedRoots = true;
	*paidTax = true;

	switch (concurrentStatus) {
	case CONCURRENT_ROOT_TRACING1:
		markingScheme_scanRoots(env);
		break;
	default:
		Assert_MM_unreachable();
	}

	return bytesScanned;
}
#endif /* OMR_GC_MODRON_CONCURRENT_MARK */

omrobjectptr_t
MM_CollectorLanguageInterfaceImpl::heapWalker_heapWalkerObjectSlotDo(omrobjectptr_t object)
{
	return NULL;
}
void
MM_CollectorLanguageInterfaceImpl::parallelGlobalGC_postMarkProcessing(MM_EnvironmentBase *env)
{
	OMR_VM *omrVM = env->getOmrVM();
	JSRuntime *rt = (JSRuntime *)omrVM->_language_vm;
	Zone *zone = OmrGcHelper::zone;
	js::AutoLockForExclusiveAccess lock(rt);

	/* Clear new object cache. Its entries may point to dead objects. */
	rt->activeContext()->caches().newObjectCache.clearNurseryObjects(rt);

	for (WeakMapBase* m : zone->gcWeakMapList()) {
		m->sweep();
	}
	for (auto* cache : zone->weakCaches()) {
		cache->sweep();
	}

	for (auto edge : zone->gcWeakRefs()) {
		/* Edges may be present multiple times, so may already be nulled. */
		if (*edge && IsAboutToBeFinalizedDuringSweep(**edge)) {
			*edge = nullptr;
		}
	}
	zone->gcWeakRefs().clear();

	/* No need to look up any more weakmap keys from this zone group. */
	AutoEnterOOMUnsafeRegion oomUnsafe;
	if (!zone->gcWeakKeys().clear()) {
		oomUnsafe.crash("clearing weak keys in beginSweepingZoneGroup()");
	}

	FreeOp fop(rt);
    
	// callFinalizeCallbacks(&fop, JSFINALIZE_GROUP_START);
    // callWeakPointerZoneGroupCallbacks();

    // for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next())
    //         callWeakPointerCompartmentCallbacks(comp);
    // }

	// for (auto& task : sweepCacheTasks) {
	// 	startTask(task, gcstats::PHASE_SWEEP_MISC, helperLock);
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
	//zone->discardJitCode(&fop);

	zone->beginSweepTypes(&fop, !zone->isPreservingCode());
	zone->sweepBreakpoints(&fop);
	zone->sweepUniqueIds(&fop);
	rt->symbolRegistry(lock).sweep();
	
	// rt->sweepAtoms(); TODO: What has this changed to?

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

	rt->gc.callFinalizeCallbacks(&fop, JSFINALIZE_GROUP_END);

	zone->types.endSweep(rt);

	/* This puts the heap into the state required to walk it */
	GC_OMRVMInterface::flushCachesForGC(env);

	MM_HeapRegionManager *regionManager = _extensions->getHeap()->getHeapRegionManager();
	{
		GC_HeapRegionIterator regionIterator(regionManager);

		/* Walk the heap for sweeping. */
		MM_HeapRegionDescriptor *hrd = regionIterator.nextRegion();
		AutoClearTypeInferenceStateOnOOM oom(zone);
		while (NULL != hrd) {
			GC_ObjectHeapIteratorAddressOrderedList objectIterator(_extensions, hrd, false);
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
	{
		GC_HeapRegionIterator regionIterator(regionManager);

		/* Walk the heap, for objects that are not marked we corrupt them to maximize the chance we will crash immediately
		if they are used. For live objects validate that they have the expected eyecatcher */
		MM_HeapRegionDescriptor *hrd = regionIterator.nextRegion();
		AutoClearTypeInferenceStateOnOOM oom(zone);
		while (NULL != hrd) {
			/* Walk all of the objects, making sure that those that were not marked are no longer
			usable. If they are later used we will know this and optimally crash */
			GC_ObjectHeapIteratorAddressOrderedList objectIterator(_extensions, hrd, false);
			omrobjectptr_t omrobjPtr = objectIterator.nextObject();
			while (NULL != omrobjPtr) {
				if (!_markingScheme->isMarked(omrobjPtr)) {
					/* object will be collected. We write the full contents of the object with a known value. */
					uintptr_t objsize = _extensions->objectModel.getConsumedSizeInBytesWithHeader(omrobjPtr);
					memset(omrobjPtr, 0x5E, (size_t)objsize);
					MM_HeapLinkedFreeHeader::fillWithHoles(omrobjPtr, objsize);
				}
				omrobjPtr = objectIterator.nextObject();
			}
			hrd = regionIterator.nextRegion();
		}
	}
}

