/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCRuntime_h
#define gc_GCRuntime_h

#include "mozilla/Atomics.h"
#include "mozilla/EnumSet.h"
#include "mozilla/Maybe.h"

#include "jsfriendapi.h"
#include "jsgc.h"

#include "gc/AtomMarking.h"
#include "gc/Heap.h"
#include "gc/Nursery.h"
#include "gc/Statistics.h"
#include "gc/StoreBuffer.h"
#include "gc/Tracer.h"
#include "js/GCAnnotations.h"
#include "js/UniquePtr.h"

namespace js {

class AutoLockGC;
class AutoLockHelperThreadState;
class VerifyPreTracer;

namespace gc {

typedef Vector<ZoneGroup*, 4, SystemAllocPolicy> ZoneGroupVector;
using BlackGrayEdgeVector = Vector<TenuredCell*, 0, SystemAllocPolicy>;

class AutoMaybeStartBackgroundAllocation;
class MarkingValidator;

enum IncrementalProgress
{
    NotFinished = 0,
    Finished
};

/*
 * GC Scheduling Overview
 * ======================
 *
 * Scheduling GC's in SpiderMonkey/Firefox is tremendously complicated because
 * of the large number of subtle, cross-cutting, and widely dispersed factors
 * that must be taken into account. A summary of some of the more important
 * factors follows.
 *
 * Cost factors:
 *
 *   * GC too soon and we'll revisit an object graph almost identical to the
 *     one we just visited; since we are unlikely to find new garbage, the
 *     traversal will be largely overhead. We rely heavily on external factors
 *     to signal us that we are likely to find lots of garbage: e.g. "a tab
 *     just got closed".
 *
 *   * GC too late and we'll run out of memory to allocate (e.g. Out-Of-Memory,
 *     hereafter simply abbreviated to OOM). If this happens inside
 *     SpiderMonkey we may be able to recover, but most embedder allocations
 *     will simply crash on OOM, even if the GC has plenty of free memory it
 *     could surrender.
 *
 *   * Memory fragmentation: if we fill the process with GC allocations, a
 *     request for a large block of contiguous memory may fail because no
 *     contiguous block is free, despite having enough memory available to
 *     service the request.
 *
 *   * Management overhead: if our GC heap becomes large, we create extra
 *     overhead when managing the GC's structures, even if the allocations are
 *     mostly unused.
 *
 * Heap Management Factors:
 *
 *   * GC memory: The GC has its own allocator that it uses to make fixed size
 *     allocations for GC managed things. In cases where the GC thing requires
 *     larger or variable sized memory to implement itself, it is responsible
 *     for using the system heap.
 *
 *   * C Heap Memory: Rather than allowing for large or variable allocations,
 *     the SpiderMonkey GC allows GC things to hold pointers to C heap memory.
 *     It is the responsibility of the thing to free this memory with a custom
 *     finalizer (with the sole exception of NativeObject, which knows about
 *     slots and elements for performance reasons). C heap memory has different
 *     performance and overhead tradeoffs than GC internal memory, which need
 *     to be considered with scheduling a GC.
 *
 * Application Factors:
 *
 *   * Most applications allocate heavily at startup, then enter a processing
 *     stage where memory utilization remains roughly fixed with a slower
 *     allocation rate. This is not always the case, however, so while we may
 *     optimize for this pattern, we must be able to handle arbitrary
 *     allocation patterns.
 *
 * Other factors:
 *
 *   * Other memory: This is memory allocated outside the purview of the GC.
 *     Data mapped by the system for code libraries, data allocated by those
 *     libraries, data in the JSRuntime that is used to manage the engine,
 *     memory used by the embedding that is not attached to a GC thing, memory
 *     used by unrelated processes running on the hardware that use space we
 *     could otherwise use for allocation, etc. While we don't have to manage
 *     it, we do have to take it into account when scheduling since it affects
 *     when we will OOM.
 *
 *   * Physical Reality: All real machines have limits on the number of bits
 *     that they are physically able to store. While modern operating systems
 *     can generally make additional space available with swapping, at some
 *     point there are simply no more bits to allocate. There is also the
 *     factor of address space limitations, particularly on 32bit machines.
 *
 *   * Platform Factors: Each OS makes use of wildly different memory
 *     management techniques. These differences result in different performance
 *     tradeoffs, different fragmentation patterns, and different hard limits
 *     on the amount of physical and/or virtual memory that we can use before
 *     OOMing.
 *
 *
 * Reasons for scheduling GC
 * -------------------------
 *
 *  While code generally takes the above factors into account in only an ad-hoc
 *  fashion, the API forces the user to pick a "reason" for the GC. We have a
 *  bunch of JS::gcreason reasons in GCAPI.h. These fall into a few categories
 *  that generally coincide with one or more of the above factors.
 *
 *  Embedding reasons:
 *
 *   1) Do a GC now because the embedding knows something useful about the
 *      zone's memory retention state. These are gcreasons like LOAD_END,
 *      PAGE_HIDE, SET_NEW_DOCUMENT, DOM_UTILS. Mostly, Gecko uses these to
 *      indicate that a significant fraction of the scheduled zone's memory is
 *      probably reclaimable.
 *
 *   2) Do some known amount of GC work now because the embedding knows now is
 *      a good time to do a long, unblockable operation of a known duration.
 *      These are INTER_SLICE_GC and REFRESH_FRAME.
 *
 *  Correctness reasons:
 *
 *   3) Do a GC now because correctness depends on some GC property. For
 *      example, CC_WAITING is where the embedding requires the mark bits
 *      to be set correct. Also, EVICT_NURSERY where we need to work on the tenured
 *      heap.
 *
 *   4) Do a GC because we are shutting down: e.g. SHUTDOWN_CC or DESTROY_*.
 *
 *   5) Do a GC because a compartment was accessed between GC slices when we
 *      would have otherwise discarded it. We have to do a second GC to clean
 *      it up: e.g. COMPARTMENT_REVIVED.
 *
 *  Emergency Reasons:
 *
 *   6) Do an all-zones, non-incremental GC now because the embedding knows it
 *      cannot wait: e.g. MEM_PRESSURE.
 *
 *   7) OOM when fetching a new Chunk results in a LAST_DITCH GC.
 *
 *  Heap Size Limitation Reasons:
 *
 *   8) Do an incremental, zonal GC with reason MAYBEGC when we discover that
 *      the gc's allocated size is approaching the current trigger. This is
 *      called MAYBEGC because we make this check in the MaybeGC function.
 *      MaybeGC gets called at the top of the main event loop. Normally, it is
 *      expected that this callback will keep the heap size limited. It is
 *      relatively inexpensive, because it is invoked with no JS running and
 *      thus few stack roots to scan. For this reason, the GC's "trigger" bytes
 *      is less than the GC's "max" bytes as used by the trigger below.
 *
 *   9) Do an incremental, zonal GC with reason MAYBEGC when we go to allocate
 *      a new GC thing and find that the GC heap size has grown beyond the
 *      configured maximum (JSGC_MAX_BYTES). We trigger this GC by returning
 *      nullptr and then calling maybeGC at the top level of the allocator.
 *      This is then guaranteed to fail the "size greater than trigger" check
 *      above, since trigger is always less than max. After performing the GC,
 *      the allocator unconditionally returns nullptr to force an OOM exception
 *      is raised by the script.
 *
 *      Note that this differs from a LAST_DITCH GC where we actually run out
 *      of memory (i.e., a call to a system allocator fails) when trying to
 *      allocate. Unlike above, LAST_DITCH GC only happens when we are really
 *      out of memory, not just when we cross an arbitrary trigger; despite
 *      this, it may still return an allocation at the end and allow the script
 *      to continue, if the LAST_DITCH GC was able to free up enough memory.
 *
 *  10) Do a GC under reason ALLOC_TRIGGER when we are over the GC heap trigger
 *      limit, but in the allocator rather than in a random call to maybeGC.
 *      This occurs if we allocate too much before returning to the event loop
 *      and calling maybeGC; this is extremely common in benchmarks and
 *      long-running Worker computations. Note that this uses a wildly
 *      different mechanism from the above in that it sets the interrupt flag
 *      and does the GC at the next loop head, before the next alloc, or
 *      maybeGC. The reason for this is that this check is made after the
 *      allocation and we cannot GC with an uninitialized thing in the heap.
 *
 *  11) Do an incremental, zonal GC with reason TOO_MUCH_MALLOC when we have
 *      malloced more than JSGC_MAX_MALLOC_BYTES in a zone since the last GC.
 *
 *
 * Size Limitation Triggers Explanation
 * ------------------------------------
 *
 *  The GC internally is entirely unaware of the context of the execution of
 *  the mutator. It sees only:
 *
 *   A) Allocated size: this is the amount of memory currently requested by the
 *      mutator. This quantity is monotonically increasing: i.e. the allocation
 *      rate is always >= 0. It is also easy for the system to track.
 *
 *   B) Retained size: this is the amount of memory that the mutator can
 *      currently reach. Said another way, it is the size of the heap
 *      immediately after a GC (modulo background sweeping). This size is very
 *      costly to know exactly and also extremely hard to estimate with any
 *      fidelity.
 *
 *   For reference, a common allocated vs. retained graph might look like:
 *
 *       |                                  **         **
 *       |                       **       ** *       **
 *       |                     ** *     **   *     **
 *       |           *       **   *   **     *   **
 *       |          **     **     * **       * **
 *      s|         * *   **       ** +  +    **
 *      i|        *  *  *      +  +       +  +     +
 *      z|       *   * * +  +                   +     +  +
 *      e|      *    **+
 *       |     *     +
 *       |    *    +
 *       |   *   +
 *       |  *  +
 *       | * +
 *       |*+
 *       +--------------------------------------------------
 *                               time
 *                                           *** = allocated
 *                                           +++ = retained
 *
 *           Note that this is a bit of a simplification
 *           because in reality we track malloc and GC heap
 *           sizes separately and have a different level of
 *           granularity and accuracy on each heap.
 *
 *   This presents some obvious implications for Mark-and-Sweep collectors.
 *   Namely:
 *       -> t[marking] ~= size[retained]
 *       -> t[sweeping] ~= size[allocated] - size[retained]
 *
 *   In a non-incremental collector, maintaining low latency and high
 *   responsiveness requires that total GC times be as low as possible. Thus,
 *   in order to stay responsive when we did not have a fully incremental
 *   collector, our GC triggers were focused on minimizing collection time.
 *   Furthermore, since size[retained] is not under control of the GC, all the
 *   GC could do to control collection times was reduce sweep times by
 *   minimizing size[allocated], per the equation above.
 *
 *   The result of the above is GC triggers that focus on size[allocated] to
 *   the exclusion of other important factors and default heuristics that are
 *   not optimal for a fully incremental collector. On the other hand, this is
 *   not all bad: minimizing size[allocated] also minimizes the chance of OOM
 *   and sweeping remains one of the hardest areas to further incrementalize.
 *
 *      EAGER_ALLOC_TRIGGER
 *      -------------------
 *      Occurs when we return to the event loop and find our heap is getting
 *      largish, but before t[marking] OR t[sweeping] is too large for a
 *      responsive non-incremental GC. This is intended to be the common case
 *      in normal web applications: e.g. we just finished an event handler and
 *      the few objects we allocated when computing the new whatzitz have
 *      pushed us slightly over the limit. After this GC we rescale the new
 *      EAGER_ALLOC_TRIGGER trigger to 150% of size[retained] so that our
 *      non-incremental GC times will always be proportional to this size
 *      rather than being dominated by sweeping.
 *
 *      As a concession to mutators that allocate heavily during their startup
 *      phase, we have a highFrequencyGCMode that ups the growth rate to 300%
 *      of the current size[retained] so that we'll do fewer longer GCs at the
 *      end of the mutator startup rather than more, smaller GCs.
 *
 *          Assumptions:
 *            -> Responsiveness is proportional to t[marking] + t[sweeping].
 *            -> size[retained] is proportional only to GC allocations.
 *
 *      ALLOC_TRIGGER (non-incremental)
 *      -------------------------------
 *      If we do not return to the event loop before getting all the way to our
 *      gc trigger bytes then MAYBEGC will never fire. To avoid OOMing, we
 *      succeed the current allocation and set the script interrupt so that we
 *      will (hopefully) do a GC before we overflow our max and have to raise
 *      an OOM exception for the script.
 *
 *          Assumptions:
 *            -> Common web scripts will return to the event loop before using
 *               10% of the current gcTriggerBytes worth of GC memory.
 *
 *      ALLOC_TRIGGER (incremental)
 *      ---------------------------
 *      In practice the above trigger is rough: if a website is just on the
 *      cusp, sometimes it will trigger a non-incremental GC moments before
 *      returning to the event loop, where it could have done an incremental
 *      GC. Thus, we recently added an incremental version of the above with a
 *      substantially lower threshold, so that we have a soft limit here. If
 *      IGC can collect faster than the allocator generates garbage, even if
 *      the allocator does not return to the event loop frequently, we should
 *      not have to fall back to a non-incremental GC.
 *
 *      INCREMENTAL_TOO_SLOW
 *      --------------------
 *      Do a full, non-incremental GC if we overflow ALLOC_TRIGGER during an
 *      incremental GC. When in the middle of an incremental GC, we suppress
 *      our other triggers, so we need a way to backstop the IGC if the
 *      mutator allocates faster than the IGC can clean things up.
 *
 *      TOO_MUCH_MALLOC
 *      ---------------
 *      Performs a GC before size[allocated] - size[retained] gets too large
 *      for non-incremental sweeping to be fast in the case that we have
 *      significantly more malloc allocation than GC allocation. This is meant
 *      to complement MAYBEGC triggers. We track this by counting malloced
 *      bytes; the counter gets reset at every GC since we do not always have a
 *      size at the time we call free. Because of this, the malloc heuristic
 *      is, unfortunately, not usefully able to augment our other GC heap
 *      triggers and is limited to this singular heuristic.
 *
 *          Assumptions:
 *            -> EITHER size[allocated_by_malloc] ~= size[allocated_by_GC]
 *                 OR   time[sweeping] ~= size[allocated_by_malloc]
 *            -> size[retained] @ t0 ~= size[retained] @ t1
 *               i.e. That the mutator is in steady-state operation.
 *
 *      LAST_DITCH_GC
 *      -------------
 *      Does a GC because we are out of memory.
 *
 *          Assumptions:
 *            -> size[retained] < size[available_memory]
 */

template<typename F>
struct Callback {
    ActiveThreadOrGCTaskData<F> op;
    ActiveThreadOrGCTaskData<void*> data;

    Callback()
      : op(nullptr), data(nullptr)
    {}
    Callback(F op, void* data)
      : op(op), data(data)
    {}
};

template<typename F>
using CallbackVector = ActiveThreadData<Vector<Callback<F>, 4, SystemAllocPolicy>>;

typedef HashMap<Value*, const char*, DefaultHasher<Value*>, SystemAllocPolicy> RootedValueMap;

using AllocKinds = mozilla::EnumSet<AllocKind>;

template <typename T>
class MemoryCounter
{
    // Bytes counter to measure memory pressure for GC scheduling. It runs
    // from maxBytes down to zero.
    mozilla::Atomic<ptrdiff_t, mozilla::ReleaseAcquire> bytes_;

    // GC trigger threshold for memory allocations.
    js::ActiveThreadData<size_t> maxBytes_;

    // Whether a GC has been triggered as a result of bytes falling below
    // zero.
    //
    // This should be a bool, but Atomic only supports 32-bit and pointer-sized
    // types.
    mozilla::Atomic<uint32_t, mozilla::ReleaseAcquire> triggered_;

  public:
    MemoryCounter()
      : bytes_(0),
        maxBytes_(0),
        triggered_(false)
    { }

    void reset() {
        bytes_ = maxBytes_;
        triggered_ = false;
    }

    void setMax(size_t newMax) {
        // For compatibility treat any value that exceeds PTRDIFF_T_MAX to
        // mean that value.
        maxBytes_ = (ptrdiff_t(newMax) >= 0) ? newMax : size_t(-1) >> 1;
        reset();
    }

    bool update(T* owner, size_t bytes) {
        bytes_ -= ptrdiff_t(bytes);
        if (MOZ_UNLIKELY(isTooMuchMalloc())) {
            if (!triggered_)
                triggered_ = owner->triggerGCForTooMuchMalloc();
        }
        return triggered_;
    }

    ptrdiff_t bytes() const { return bytes_; }
    size_t maxBytes() const { return maxBytes_; }
    bool isTooMuchMalloc() const { return bytes_ <= 0; }
};

class GCRuntime
{
  public:
    explicit GCRuntime(JSRuntime* rt);
    MOZ_MUST_USE bool init(uint32_t maxbytes, uint32_t maxNurseryBytes);
    void finishRoots();
    void finish();

    inline bool hasZealMode(ZealMode mode);
    inline void clearZealMode(ZealMode mode);
    inline bool upcomingZealousGC();
    inline bool needZealousGC();

    MOZ_MUST_USE bool addRoot(Value* vp, const char* name);
    void removeRoot(Value* vp);

    MOZ_MUST_USE bool setParameter(JSGCParamKey key, uint32_t value, AutoLockGC& lock);
    void resetParameter(JSGCParamKey key, AutoLockGC& lock);
    uint32_t getParameter(JSGCParamKey key, const AutoLockGC& lock);

    void maybeGC(Zone* zone);
    // The return value indicates whether a major GC was performed.
    bool gcIfRequested();
    void gc(JSGCInvocationKind gckind, JS::gcreason::Reason reason) {}
    void startDebugGC(JSGCInvocationKind gckind, SliceBudget& budget);
    void debugGCSlice(SliceBudget& budget);

    bool canChangeActiveContext(JSContext* cx);

    void triggerFullGCForAtoms(JSContext* cx);

    enum TraceOrMarkRuntime {
        TraceRuntime,
        MarkRuntime
    };
    void traceRuntime(JSTracer* trc, AutoLockForExclusiveAccess& lock);
    void traceRuntimeForMinorGC(JSTracer* trc, AutoLockForExclusiveAccess& lock);

    void notifyDidPaint();
    void onOutOfMallocMemory();

#ifdef JS_GC_ZEAL
    const void* addressOfZealModeBits() { return &zealModeBits; }
    void getZealBits(uint32_t* zealBits, uint32_t* frequency, uint32_t* nextScheduled);
    void setZeal(uint8_t zeal, uint32_t frequency);
    bool parseAndSetZeal(const char* str);
    void setNextScheduled(uint32_t count);
    bool selectForMarking(JSObject* object);
    void setDeterministic(bool enable);
#endif

    uint64_t nextCellUniqueId() {
        MOZ_ASSERT(nextCellUniqueId_ > 0);
        uint64_t uid = ++nextCellUniqueId_;
        return uid;
    }

#ifdef DEBUG
    bool shutdownCollectedEverything() const {
        return true;
    }
#endif

  public:
    // Internal public interface
    State state() const { return incrementalState; }
    bool isHeapCompacting() const { return false; }
    bool isForegroundSweeping() const { return false; }
    void waitBackgroundSweepEnd() { }
    void waitBackgroundSweepOrAllocEnd() { }

#ifdef DEBUG
    bool onBackgroundThread() { return false; }
#endif // DEBUG

    void lockGC() {
        lock.lock();
    }

    void unlockGC() {
        lock.unlock();
    }

#ifdef DEBUG
    bool currentThreadHasLockedGC() const {
        return lock.ownedByCurrentThread();
    }
#endif // DEBUG

    void setAlwaysPreserveCode() {}

    bool isIncrementalGCAllowed() const { return false; }
    void disallowIncrementalGC() { }
    bool isIncrementalGCInProgress() const { return false; }
	
	bool isShrinkingGC() const { return false; }

    bool isShrinkingGC() const { return invocationKind == GC_SHRINK; }

    bool initSweepActions();

    void setGrayRootsTracer(JSTraceDataOp traceOp, void* data);
    MOZ_MUST_USE bool addBlackRootsTracer(JSTraceDataOp traceOp, void* data);
    void removeBlackRootsTracer(JSTraceDataOp traceOp, void* data);

    void updateMallocCounter(JS::Zone* zone, size_t nbytes);

    void setGCCallback(JSGCCallback callback, void* data);
    void callGCCallback(JSGCStatus status) const;
    void setObjectsTenuredCallback(JSObjectsTenuredCallback callback,
                                   void* data);
    MOZ_MUST_USE bool addFinalizeCallback(JSFinalizeCallback callback, void* data);
    void removeFinalizeCallback(JSFinalizeCallback func);
    MOZ_MUST_USE bool addWeakPointerZonesCallback(JSWeakPointerZonesCallback callback,
                                                      void* data);
    void removeWeakPointerZonesCallback(JSWeakPointerZonesCallback callback);
    MOZ_MUST_USE bool addWeakPointerCompartmentCallback(JSWeakPointerCompartmentCallback callback,
                                                        void* data);
    void removeWeakPointerCompartmentCallback(JSWeakPointerCompartmentCallback callback);

    void setFullCompartmentChecks(bool enable);

    JS::Zone* getCurrentSweepGroup() { return currentSweepGroup; }

    uint64_t number;
    uint64_t gcNumber() const { return number; }
    uint64_t minorGCCount() const { return number; }
    uint64_t majorGCCount() const { return number; }
	void incGcNumber() { number ++; }
 
    bool isFullGc() const { return false; }
    bool areGrayBitsValid() const { return false; }
    bool fullGCForAtomsRequested() const { return false; }
    bool isVerifyPreBarriersEnabled() const { return false; }

    // Free certain LifoAlloc blocks when it is safe to do so.
    void freeUnusedLifoBlocksAfterSweeping(LifoAlloc* lifo);
    void freeAllLifoBlocksAfterSweeping(LifoAlloc* lifo);

    // Allocator
    template <AllowGC allowGC>
    MOZ_MUST_USE bool checkAllocatorState(JSContext* cx, AllocKind kind);

  public:
    void traceRuntimeAtoms(JSTracer* trc, AutoLockForExclusiveAccess& lock);
    void traceRuntimeCommon(JSTracer* trc, TraceOrMarkRuntime traceOrMark,
                            AutoLockForExclusiveAccess& lock);

    void callFinalizeCallbacks(FreeOp* fop, JSFinalizeStatus status) const;

  public:
    JSRuntime* const rt;

    /* Embedders can use this zone and group however they wish. */
    UnprotectedData<JS::Zone*> systemZone;
    UnprotectedData<ZoneGroup*> systemZoneGroup;

    // List of all zone groups (protected by the GC lock).

  private:
    ActiveThreadOrGCTaskData<ZoneGroupVector> groups_;
  public:
    ZoneGroupVector& groups() { return groups_.ref(); }

    // The unique atoms zone, which has no zone group.
    WriteOnceData<Zone*> atomsZone;

  private:
    UnprotectedData<gcstats::Statistics> stats_;

  public:
    gcstats::Statistics& stats() { return stats_.ref(); }

    GCMarker marker;

    // State used for managing atom mark bitmaps in each zone. Protected by the
    // exclusive access lock.
    AtomMarkingRuntime atomMarking;

  private:
    ActiveThreadData<RootedValueMap> rootsHash;

    // An incrementing id used to assign unique ids to cells that require one.
    mozilla::Atomic<uint64_t, mozilla::ReleaseAcquire> nextCellUniqueId_;

    /* Whether all zones are being collected in first GC slice. */
    ActiveThreadData<bool> isFull;

    /*
     * The current incremental GC phase. This is also used internally in
     * non-incremental GC.
     */
    ActiveThreadOrGCTaskData<State> incrementalState;

    /*
     * Incremental sweep state.
     */
    ActiveThreadData<JS::Zone*> sweepGroups;
    ActiveThreadOrGCTaskData<JS::Zone*> currentSweepGroup;
    ActiveThreadData<UniquePtr<SweepAction<GCRuntime*, FreeOp*, SliceBudget&>>> sweepActions;
    ActiveThreadOrGCTaskData<JS::Zone*> sweepZone;
    ActiveThreadData<mozilla::Maybe<AtomSet::Enum>> maybeAtomsToSweep;
    ActiveThreadOrGCTaskData<JS::detail::WeakCacheBase*> sweepCache;
    ActiveThreadData<bool> abortSweepAfterCurrentGroup;

    /*
     * Whether compacting GC can is enabled globally.
     *
     * JSGC_COMPACTING_ENABLED
     * pref: javascript.options.mem.gc_compacting
     */
    ActiveThreadData<bool> compactingEnabled;

    ActiveThreadData<bool> rootsRemoved;

    /*
     * These options control the zealousness of the GC. At every allocation,
     * nextScheduled is decremented. When it reaches zero we do a full GC.
     *
     * At this point, if zeal_ is one of the types that trigger periodic
     * collection, then nextScheduled is reset to the value of zealFrequency.
     * Otherwise, no additional GCs take place.
     *
     * You can control these values in several ways:
     *   - Set the JS_GC_ZEAL environment variable
     *   - Call gczeal() or schedulegc() from inside shell-executed JS code
     *     (see the help for details)
     *
     * If gcZeal_ == 1 then we perform GCs in select places (during MaybeGC and
     * whenever we are notified that GC roots have been removed). This option is
     * mainly useful to embedders.
     *
     * We use zeal_ == 4 to enable write barrier verification. See the comment
     * in jsgc.cpp for more information about this.
     *
     * zeal_ values from 8 to 10 periodically run different types of
     * incremental GC.
     *
     * zeal_ value 14 performs periodic shrinking collections.
     */
#ifdef JS_GC_ZEAL
    ActiveThreadData<uint32_t> zealModeBits;
    ActiveThreadData<int> nextScheduled;
#endif
    Callback<JSGCCallback> gcCallback;
    CallbackVector<JSFinalizeCallback> finalizeCallbacks;

    /*
     * The trace operations to trace embedding-specific GC roots. One is for
     * tracing through black roots and the other is for tracing through gray
     * roots. The black/gray distinction is only relevant to the cycle
     * collector.
     */
    CallbackVector<JSTraceDataOp> blackRootTracers;
    Callback<JSTraceDataOp> grayRootTracer;

    /* Synchronize GC heap access among GC helper threads and active threads. */
    friend class js::AutoLockGC;
    js::Mutex lock;

  private:
    ActiveThreadData<Nursery> nursery_;
  public:
    Nursery& nursery() { return nursery_.ref(); }

    const void* addressOfNurseryPosition() {
        return nursery_.refNoCheck().addressOfPosition();
    }
    const void* addressOfNurseryCurrentEnd() {
        return nursery_.refNoCheck().addressOfCurrentEnd();
    }

    void minorGC(JS::gcreason::Reason reason,
                 gcstats::Phase phase = gcstats::PHASE_MINOR_GC) JS_HAZ_GC_CALL;
    void evictNursery(JS::gcreason::Reason reason = JS::gcreason::EVICT_NURSERY) {
        minorGC(reason, gcstats::PHASE_EVICT_NURSERY);
    }
    void freeAllLifoBlocksAfterMinorGC(LifoAlloc* lifo);

    int enabled;

    void disable() {
		enabled --;
	}

    void enable() {
		enabled ++;
	}

    friend class MarkingValidator;
    friend class AutoEnterIteration;
};

/* Prevent compartments and zones from being collected during iteration. */
class MOZ_RAII AutoEnterIteration {
    GCRuntime* gc;

  public:
    explicit AutoEnterIteration(GCRuntime* gc_) : gc(gc_) {
        ++gc->numActiveZoneIters;
    }

    ~AutoEnterIteration() {
        MOZ_ASSERT(gc->numActiveZoneIters);
        --gc->numActiveZoneIters;
    }
};

#ifdef JS_GC_ZEAL

inline bool
GCRuntime::hasZealMode(ZealMode mode) {
	return false;
}

inline bool
GCRuntime::upcomingZealousGC() {
	return false;
}

inline bool
GCRuntime::needZealousGC() {
    return false;
}
#else
inline bool GCRuntime::hasZealMode(ZealMode mode) { return false; }
inline void GCRuntime::clearZealMode(ZealMode mode) { }
inline bool GCRuntime::upcomingZealousGC() { return false; }
inline bool GCRuntime::needZealousGC() { return false; }
#endif

} /* namespace gc */

} /* namespace js */

#endif
