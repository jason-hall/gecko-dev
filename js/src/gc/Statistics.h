/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Statistics_h
#define gc_Statistics_h

#include "mozilla/Array.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/IntegerRange.h"
#include "mozilla/Maybe.h"
#include "mozilla/PodOperations.h"

#include "jsalloc.h"
#include "jsgc.h"
#include "jspubtd.h"

#include "js/GCAPI.h"
#include "js/Vector.h"

using mozilla::Maybe;

namespace js {

class GCParallelTask;

namespace gcstats {

enum Phase : uint8_t {
    PHASE_FIRST,

    PHASE_MUTATOR = PHASE_FIRST,
    PHASE_GC_BEGIN,
    PHASE_WAIT_BACKGROUND_THREAD,
    PHASE_MARK_DISCARD_CODE,
    PHASE_RELAZIFY_FUNCTIONS,
    PHASE_PURGE,
    PHASE_MARK,
    PHASE_UNMARK,
    PHASE_MARK_DELAYED,
    PHASE_SWEEP,
    PHASE_SWEEP_MARK,
    PHASE_SWEEP_MARK_TYPES,
    PHASE_SWEEP_MARK_INCOMING_BLACK,
    PHASE_SWEEP_MARK_WEAK,
    PHASE_SWEEP_MARK_INCOMING_GRAY,
    PHASE_SWEEP_MARK_GRAY,
    PHASE_SWEEP_MARK_GRAY_WEAK,
    PHASE_FINALIZE_START,
    PHASE_WEAK_ZONES_CALLBACK,
    PHASE_WEAK_COMPARTMENT_CALLBACK,
    PHASE_SWEEP_ATOMS,
    PHASE_SWEEP_COMPARTMENTS,
    PHASE_SWEEP_DISCARD_CODE,
    PHASE_SWEEP_INNER_VIEWS,
    PHASE_SWEEP_CC_WRAPPER,
    PHASE_SWEEP_BASE_SHAPE,
    PHASE_SWEEP_INITIAL_SHAPE,
    PHASE_SWEEP_TYPE_OBJECT,
    PHASE_SWEEP_BREAKPOINT,
    PHASE_SWEEP_REGEXP,
    PHASE_SWEEP_MISC,
    PHASE_SWEEP_TYPES,
    PHASE_SWEEP_TYPES_BEGIN,
    PHASE_SWEEP_TYPES_END,
    PHASE_SWEEP_OBJECT,
    PHASE_SWEEP_STRING,
    PHASE_SWEEP_SCRIPT,
    PHASE_SWEEP_SCOPE,
    PHASE_SWEEP_REGEXP_SHARED,
    PHASE_SWEEP_SHAPE,
    PHASE_SWEEP_JITCODE,
    PHASE_FINALIZE_END,
    PHASE_DESTROY,
    PHASE_COMPACT,
    PHASE_COMPACT_MOVE,
    PHASE_COMPACT_UPDATE,
    PHASE_COMPACT_UPDATE_CELLS,
    PHASE_GC_END,
    PHASE_MINOR_GC,
    PHASE_EVICT_NURSERY,
    PHASE_TRACE_HEAP,
    PHASE_BARRIER,
    PHASE_UNMARK_GRAY,
    PHASE_MARK_ROOTS,
    PHASE_BUFFER_GRAY_ROOTS,
    PHASE_MARK_CCWS,
    PHASE_MARK_STACK,
    PHASE_MARK_RUNTIME_DATA,
    PHASE_MARK_EMBEDDING,
    PHASE_MARK_COMPARTMENTS,
    PHASE_PURGE_SHAPE_TABLES,

    PHASE_LIMIT,
    PHASE_NONE = PHASE_LIMIT,
    PHASE_EXPLICIT_SUSPENSION = PHASE_LIMIT,
    PHASE_IMPLICIT_SUSPENSION,
    PHASE_MULTI_PARENTS
};

enum Stat {
    STAT_NEW_CHUNK,
    STAT_DESTROY_CHUNK,
    STAT_MINOR_GC,

    // Number of times a 'put' into a storebuffer overflowed, triggering a
    // compaction
    STAT_STOREBUFFER_OVERFLOW,

    // Number of arenas relocated by compacting GC.
    STAT_ARENA_RELOCATED,

    STAT_LIMIT
};

/*
 * Struct for collecting timing statistics on a "phase tree". The tree is
 * specified as a limited DAG, but the timings are collected for the whole tree
 * that you would get by expanding out the DAG by duplicating subtrees rooted
 * at nodes with multiple parents.
 *
 * During execution, a child phase can be activated multiple times, and the
 * total time will be accumulated. (So for example, you can start and end
 * PHASE_MARK_ROOTS multiple times before completing the parent phase.)
 *
 * Incremental GC is represented by recording separate timing results for each
 * slice within the overall GC.
 */
struct Statistics
{
    template <typename T, size_t Length>
    using Array = mozilla::Array<T, Length>;

    template <typename IndexType, IndexType SizeAsEnumValue, typename ValueType>
    using EnumeratedArray = mozilla::EnumeratedArray<IndexType, SizeAsEnumValue, ValueType>;

    using TimeDuration = mozilla::TimeDuration;
    using TimeStamp = mozilla::TimeStamp;

    /*
     * Phases are allowed to have multiple parents, though any path from root
     * to leaf is allowed at most one multi-parented phase. We keep a full set
     * of timings for each of the multi-parented phases, to be able to record
     * all the timings in the expanded tree induced by our dag.
     *
     * Note that this wastes quite a bit of space, since we have a whole
     * separate array of timing data containing all the phases. We could be
     * more clever and keep an array of pointers biased by the offset of the
     * multi-parented phase, and thereby preserve the simple
     * timings[slot][PHASE_*] indexing. But the complexity doesn't seem worth
     * the few hundred bytes of savings. If we want to extend things to full
     * DAGs, this decision should be reconsidered.
     */
    static const size_t MaxMultiparentPhases = 6;
    static const size_t NumTimingArrays = MaxMultiparentPhases + 1;

    /* Create a convenient type for referring to tables of phase times. */
    using PhaseTimeTable =
        Array<EnumeratedArray<Phase, PHASE_LIMIT, TimeDuration>, NumTimingArrays>;

    static MOZ_MUST_USE bool initialize() { return true; }

    explicit Statistics(JSRuntime* rt) {}
    ~Statistics() {}

    MOZ_MUST_USE bool startTimingMutator() { return true; }
    MOZ_MUST_USE bool stopTimingMutator(double& mutator_ms, double& gc_ms) { return true; }

    void reset(const char* reason) {
    }
    const char* nonincrementalReason() const { return ""; }

    void count(Stat s) {
    }

    TimeDuration clearMaxGCPauseAccumulator() { return 0; }
    TimeDuration getMaxGCPauseSinceClear() { return 0; }

    static const size_t MAX_NESTING = 20;

    struct SliceData {
        SliceData(SliceBudget budget, JS::gcreason::Reason reason,
                  TimeStamp start, size_t startFaults, gc::State initialState)
          : budget(budget), reason(reason),
            initialState(initialState),
            finalState(gc::State::NotActive),
            resetReason(gc::AbortReason::None),
            start(start),
            startFaults(startFaults)
        {}

        SliceBudget budget;
        JS::gcreason::Reason reason;
        gc::State initialState, finalState;
        gc::AbortReason resetReason;
        TimeStamp start, end;
        size_t startFaults, endFaults;
        PhaseTimeTable phaseTimes;

        TimeDuration duration() const { return end - start; }
        bool wasReset() const { return resetReason != gc::AbortReason::None; }
    };

    typedef Vector<SliceData, 8, SystemAllocPolicy> SliceDataVector;
    typedef SliceDataVector::ConstRange SliceRange;

    SliceRange sliceRange() const { return slices.all(); }

    /* Print total profile times on shutdown. */
    void printTotalProfileTimes() {}

  private:
    SliceDataVector slices;
};

struct MOZ_RAII AutoPhase
{
    AutoPhase(Statistics& stats, Phase phase)
    {
    }

    AutoPhase(Statistics& stats, bool condition, Phase phase)
    {
    }

    AutoPhase(Statistics& stats, const GCParallelTask& task, Phase phase)
    {
    }

    ~AutoPhase() {
    }
};

} /* namespace gcstats */
} /* namespace js */

#endif /* gc_Statistics_h */
