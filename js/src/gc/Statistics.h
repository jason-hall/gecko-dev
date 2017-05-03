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

    int64_t clearMaxGCPauseAccumulator() { return 0; }
    int64_t getMaxGCPauseSinceClear() { return 0; }

    static const size_t MAX_NESTING = 20;

    struct SliceData {
        SliceData(SliceBudget budget, JS::gcreason::Reason reason, int64_t start,
                  double startTimestamp, size_t startFaults, gc::State initialState)
          : budget(budget), reason(reason),
            initialState(initialState),
            finalState(gc::State::NotActive),
            resetReason(nullptr),
            start(start), startTimestamp(startTimestamp),
            startFaults(startFaults)
        {
        }

        SliceBudget budget;
        JS::gcreason::Reason reason;
        gc::State initialState, finalState;
        const char* resetReason;
        int64_t start, end;
        double startTimestamp, endTimestamp;
        size_t startFaults, endFaults;
	
        int64_t duration() const { return end - start; }
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
      : stats(stats), task(nullptr), phase(phase), enabled(true)
    {
        stats.beginPhase(phase);
    }

    AutoPhase(Statistics& stats, bool condition, Phase phase)
      : stats(stats), task(nullptr), phase(phase), enabled(condition)
    {
        if (enabled)
            stats.beginPhase(phase);
    }

    AutoPhase(Statistics& stats, const GCParallelTask& task, Phase phase)
      : stats(stats), task(&task), phase(phase), enabled(true)
    {
        if (enabled)
            stats.beginPhase(phase);
    }

    ~AutoPhase() {
        if (enabled) {
            // Bug 1309651 - we only record active thread time (including time
            // spent waiting to join with helper threads), but should start
            // recording total work on helper threads sometime by calling
            // endParallelPhase here if task is nonnull.
            stats.endPhase(phase);
        }
    }

    Statistics& stats;
    const GCParallelTask* task;
    Phase phase;
    bool enabled;
};

struct MOZ_RAII AutoSCC
{
    AutoSCC(Statistics& stats, unsigned scc)
      : stats(stats), scc(scc)
    {
        start = stats.beginSCC();
    }
    ~AutoSCC() {
        stats.endSCC(scc, start);
    }

    Statistics& stats;
    unsigned scc;
    mozilla::TimeStamp start;
};

} /* namespace gcstats */
} /* namespace js */

#endif /* gc_Statistics_h */
