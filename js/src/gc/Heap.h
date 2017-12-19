/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Heap_h
#define gc_Heap_h

#include "mozilla/ArrayUtils.h"
#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/EnumeratedRange.h"
#include "mozilla/PodOperations.h"

#include <stddef.h>
#include <stdint.h>

#include "jsfriendapi.h"
#include "jspubtd.h"
#include "jstypes.h"
#include "jsutil.h"

#include "ds/BitArray.h"
#include "gc/Memory.h"
#include "js/GCAPI.h"
#include "js/HeapAPI.h"
#include "js/RootingAPI.h"
#include "js/TracingAPI.h"

/*#include "omrglue.hpp"*/
#include "CollectorLanguageInterfaceImpl.hpp"
#include "EnvironmentStandard.hpp"
#include "StandardWriteBarrier.hpp"

struct JSRuntime;

namespace omrjs {

// OMRTODO: Fix this hack.
extern OMR_VMThread *omrVMThread;
extern OMR_VM *omrVM;

}

namespace js {

class AutoLockGC;
class FreeOp;

#ifdef DEBUG

// Barriers can't be triggered during backend Ion compilation, which may run on
// a helper thread.
extern bool
CurrentThreadIsIonCompiling();
#endif

extern void
TraceManuallyBarrieredGenericPointerEdge(JSTracer* trc, gc::Cell** thingp, const char* name);

namespace gc {

class TenuredCell;
extern bool IsMarkedCell(const TenuredCell* const thingp);

/*
 * This flag allows an allocation site to request a specific heap based upon the
 * estimated lifetime or lifetime requirements of objects allocated from that
 * site.
 */
enum InitialHeap {
    DefaultHeap,
    TenuredHeap
};

/* The GC allocation kinds. */
// FIXME: uint8_t would make more sense for the underlying type, but causes
// miscompilations in GCC (fixed in 4.8.5 and 4.9.3). See also bug 1143966.
enum class AllocKind : uintptr_t {
    FIRST,
    OBJECT_FIRST = FIRST,
    FUNCTION = FIRST,
    FUNCTION_EXTENDED,
    OBJECT0,
    OBJECT0_BACKGROUND,
    OBJECT2,
    OBJECT2_BACKGROUND,
    OBJECT4,
    OBJECT4_BACKGROUND,
    OBJECT8,
    OBJECT8_BACKGROUND,
    OBJECT12,
    OBJECT12_BACKGROUND,
    OBJECT16,
    OBJECT16_BACKGROUND,
    OBJECT_LIMIT,
    OBJECT_LAST = OBJECT_LIMIT - 1,
    SCRIPT,
    LAZY_SCRIPT,
    SHAPE,
    ACCESSOR_SHAPE,
    BASE_SHAPE,
    OBJECT_GROUP,
    FAT_INLINE_STRING,
    STRING,
    EXTERNAL_STRING,
    FAT_INLINE_ATOM,
    ATOM,
    SYMBOL,
    JITCODE,
    SCOPE,
    REGEXP_SHARED,
    LIMIT,
    LAST = LIMIT - 1
};

// Macro to enumerate the different allocation kinds supplying information about
// the trace kind, C++ type and allocation size.
#define FOR_EACH_OBJECT_ALLOCKIND(D) \
 /* AllocKind              TraceKind      TypeName           SizedType */ \
    D(FUNCTION,            Object,        JSObject,          JSFunction) \
    D(FUNCTION_EXTENDED,   Object,        JSObject,          FunctionExtended) \
    D(OBJECT0,             Object,        JSObject,          JSObject_Slots0) \
    D(OBJECT0_BACKGROUND,  Object,        JSObject,          JSObject_Slots0) \
    D(OBJECT2,             Object,        JSObject,          JSObject_Slots2) \
    D(OBJECT2_BACKGROUND,  Object,        JSObject,          JSObject_Slots2) \
    D(OBJECT4,             Object,        JSObject,          JSObject_Slots4) \
    D(OBJECT4_BACKGROUND,  Object,        JSObject,          JSObject_Slots4) \
    D(OBJECT8,             Object,        JSObject,          JSObject_Slots8) \
    D(OBJECT8_BACKGROUND,  Object,        JSObject,          JSObject_Slots8) \
    D(OBJECT12,            Object,        JSObject,          JSObject_Slots12) \
    D(OBJECT12_BACKGROUND, Object,        JSObject,          JSObject_Slots12) \
    D(OBJECT16,            Object,        JSObject,          JSObject_Slots16) \
    D(OBJECT16_BACKGROUND, Object,        JSObject,          JSObject_Slots16)

#define FOR_EACH_NONOBJECT_ALLOCKIND(D) \
 /* AllocKind              TraceKind      TypeName           SizedType */ \
    D(SCRIPT,              Script,        JSScript,          JSScript) \
    D(LAZY_SCRIPT,         LazyScript,    js::LazyScript,    js::LazyScript) \
    D(SHAPE,               Shape,         js::Shape,         js::Shape) \
    D(ACCESSOR_SHAPE,      Shape,         js::AccessorShape, js::AccessorShape) \
    D(BASE_SHAPE,          BaseShape,     js::BaseShape,     js::BaseShape) \
    D(OBJECT_GROUP,        ObjectGroup,   js::ObjectGroup,   js::ObjectGroup) \
    D(FAT_INLINE_STRING,   String,        JSFatInlineString, JSFatInlineString) \
    D(STRING,              String,        JSString,          JSString) \
    D(EXTERNAL_STRING,     String,        JSExternalString,  JSExternalString) \
    D(FAT_INLINE_ATOM,     String,        js::FatInlineAtom, js::FatInlineAtom) \
    D(ATOM,                String,        js::NormalAtom,    js::NormalAtom) \
    D(SYMBOL,              Symbol,        JS::Symbol,        JS::Symbol) \
    D(JITCODE,             JitCode,       js::jit::JitCode,  js::jit::JitCode) \
    D(SCOPE,               Scope,         js::Scope,         js::Scope) \
    D(REGEXP_SHARED,       RegExpShared,  js::RegExpShared,  js::RegExpShared)

#define FOR_EACH_ALLOCKIND(D) \
    FOR_EACH_OBJECT_ALLOCKIND(D) \
    FOR_EACH_NONOBJECT_ALLOCKIND(D)

static_assert(int(AllocKind::FIRST) == 0, "Various places depend on AllocKind starting at 0, "
                                          "please audit them carefully!");
static_assert(int(AllocKind::OBJECT_FIRST) == 0, "Various places depend on AllocKind::OBJECT_FIRST "
                                                 "being 0, please audit them carefully!");

inline bool
IsObjectAllocKind(AllocKind kind)
{
    return kind >= AllocKind::OBJECT_FIRST && kind <= AllocKind::OBJECT_LAST;
}

inline bool
IsShapeAllocKind(AllocKind kind)
{
    return kind == AllocKind::SHAPE || kind == AllocKind::ACCESSOR_SHAPE;
}

// Returns a sequence for use in a range-based for loop,
// to iterate over all alloc kinds.
inline decltype(mozilla::MakeEnumeratedRange(AllocKind::FIRST, AllocKind::LIMIT))
AllAllocKinds()
{
    return mozilla::MakeEnumeratedRange(AllocKind::FIRST, AllocKind::LIMIT);
}

// Returns a sequence for use in a range-based for loop,
// to iterate over all object alloc kinds.
inline decltype(mozilla::MakeEnumeratedRange(AllocKind::OBJECT_FIRST, AllocKind::OBJECT_LIMIT))
ObjectAllocKinds()
{
    return mozilla::MakeEnumeratedRange(AllocKind::OBJECT_FIRST, AllocKind::OBJECT_LIMIT);
}

// Returns a sequence for use in a range-based for loop,
// to iterate over alloc kinds from |first| to |limit|, exclusive.
inline decltype(mozilla::MakeEnumeratedRange(AllocKind::FIRST, AllocKind::LIMIT))
SomeAllocKinds(AllocKind first = AllocKind::FIRST, AllocKind limit = AllocKind::LIMIT)
{
    return mozilla::MakeEnumeratedRange(first, limit);
}

// AllAllocKindArray<ValueType> gives an enumerated array of ValueTypes,
// with each index corresponding to a particular alloc kind.
template<typename ValueType> using AllAllocKindArray =
    mozilla::EnumeratedArray<AllocKind, AllocKind::LIMIT, ValueType>;

// ObjectAllocKindArray<ValueType> gives an enumerated array of ValueTypes,
// with each index corresponding to a particular object alloc kind.
template<typename ValueType> using ObjectAllocKindArray =
    mozilla::EnumeratedArray<AllocKind, AllocKind::OBJECT_LIMIT, ValueType>;

static inline JS::TraceKind
MapAllocToTraceKind(AllocKind kind)
{
    static const JS::TraceKind map[] = {
#define EXPAND_ELEMENT(allocKind, traceKind, type, sizedType) \
        JS::TraceKind::traceKind,
FOR_EACH_ALLOCKIND(EXPAND_ELEMENT)
#undef EXPAND_ELEMENT
    };

    static_assert(MOZ_ARRAY_LENGTH(map) == size_t(AllocKind::LIMIT),
                  "AllocKind-to-TraceKind mapping must be in sync");
    return map[size_t(kind)];
}

/* Mark colors to pass to markIfUnmarked. */
enum class MarkColor : uint32_t
{
    Black = 0,
    Gray
};

class TenuredCell;

// A GC cell is the base class for all GC things.
struct Cell
{
  public:
    using Flags = uintptr_t;

  public:
    MOZ_ALWAYS_INLINE bool isTenured() const { return !IsInsideNursery(this); }
    MOZ_ALWAYS_INLINE const TenuredCell& asTenured() const;
    MOZ_ALWAYS_INLINE TenuredCell& asTenured();

    MOZ_ALWAYS_INLINE bool isMarkedAny() const { return false; }
    MOZ_ALWAYS_INLINE bool isMarkedBlack() const { return false; }
    MOZ_ALWAYS_INLINE bool isMarkedGray() const { return false; }

    inline JS::Zone* zoneFromAnyThread() const;
    inline JS::Zone* zone() const;

    inline JSRuntime* runtimeFromActiveCooperatingThread() const;

    // Note: Unrestricted access to the runtime of a GC thing from an arbitrary
    // thread can easily lead to races. Use this method very carefully.
    inline JSRuntime* runtimeFromAnyThread() const;

    // May be overridden by GC thing kinds that have a compartment pointer.
    inline JSCompartment* maybeCompartment() const { return nullptr; }

    inline JS::TraceKind getTraceKind() const;

    inline AllocKind getAllocKind() const { MOZ_ASSERT(((flags_ >> 2) & 829952) == 829952); return (AllocKind)((flags_ >> 2) & ~829952); }
    inline void setAllocKind(AllocKind allocKind) { flags_ = (Flags)((((int)allocKind) | 829952) << 2); }

    static MOZ_ALWAYS_INLINE bool needWriteBarrierPre(JS::Zone* zone);

#ifdef DEBUG
    inline bool isAligned() const;
    void dump(FILE* fp) const;
    void dump() const;
#endif

  protected:
    inline uintptr_t address() const;

  public:
    Flags flags_;
} JS_HAZ_GC_THING;

// A GC TenuredCell gets behaviors that are valid for things in the Tenured
// heap, such as access to the arena and mark bits.
class TenuredCell : public Cell
{
  public:
    // Construct a TenuredCell from a void*, making various sanity assertions.
    static MOZ_ALWAYS_INLINE TenuredCell* fromPointer(void* ptr);
    static MOZ_ALWAYS_INLINE const TenuredCell* fromPointer(const void* ptr);

    // Mark bit management.
    MOZ_ALWAYS_INLINE bool isMarkedAny() const;
    MOZ_ALWAYS_INLINE bool isMarkedBlack() const;
    MOZ_ALWAYS_INLINE bool isMarkedGray() const;

    // The return value indicates if the cell went from unmarked to marked.
    MOZ_ALWAYS_INLINE bool markIfUnmarked(MarkColor color = MarkColor::Black) const;
    MOZ_ALWAYS_INLINE void markBlack() const;
    MOZ_ALWAYS_INLINE void copyMarkBitsFrom(const TenuredCell* src);

    inline JS::TraceKind getTraceKind() const;

    static MOZ_ALWAYS_INLINE void readBarrier(TenuredCell* thing);
    static MOZ_ALWAYS_INLINE void writeBarrierPre(TenuredCell* thing);

    static MOZ_ALWAYS_INLINE void writeBarrierPost(void* cellp, TenuredCell* prior,
                                                   TenuredCell* next);

    // Default implementation for kinds that don't require fixup.
    void fixupAfterMovingGC() {}

#ifdef DEBUG
    inline bool isAligned() const;
#endif
};

/* Cells are aligned to CellAlignShift, so the largest tagged null pointer is: */
const uintptr_t LargestTaggedNullCellPointer = (1 << CellAlignShift) - 1;

/*
 * The minimum cell size ends up as twice the cell alignment because the mark
 * bitmap contains one bit per CellBytesPerMarkBit bytes (which is equal to
 * CellAlignBytes) and we need two mark bits per cell.
 */
const size_t MarkBitsPerCell = 2;
const size_t MinCellSize = CellBytesPerMarkBit * MarkBitsPerCell;


class FreeSpan
{
  public:
    static size_t offsetOfFirst() {
        return 0;
    }

    static size_t offsetOfLast() {
        return 0;
    }
};

MOZ_ALWAYS_INLINE const TenuredCell&
Cell::asTenured() const
{
    return *static_cast<const TenuredCell*>(this);
}

MOZ_ALWAYS_INLINE TenuredCell&
Cell::asTenured()
{
    return *static_cast<TenuredCell*>(this);
}

// OMRTOO: Getting Runtime with context

inline JSRuntime*
Cell::runtimeFromActiveCooperatingThread() const
{
    return reinterpret_cast<JS::shadow::Zone*>(zone())->runtimeFromActiveCooperatingThread();
}

inline JSRuntime*
Cell::runtimeFromAnyThread() const
{
    return reinterpret_cast<JS::shadow::Zone*>(zone())->runtimeFromAnyThread();
}

inline JS::Zone*
Cell::zoneFromAnyThread() const
{
    // OMRTODO: Proper zones
    return OmrGcHelper::zone;
}

inline JS::Zone*
Cell::zone() const
{
    // OMRTODO: Use multiple zones obtained from a thread context
    return OmrGcHelper::zone;
}

inline uintptr_t
Cell::address() const
{
	return uintptr_t(this);
}

inline JS::TraceKind
Cell::getTraceKind() const
{
    switch (getAllocKind())
	{
		case AllocKind::OBJECT0:
		case AllocKind::OBJECT0_BACKGROUND:
		case AllocKind::OBJECT2:
		case AllocKind::OBJECT2_BACKGROUND:
		case AllocKind::OBJECT4:
		case AllocKind::OBJECT4_BACKGROUND:
		case AllocKind::OBJECT8:
		case AllocKind::OBJECT8_BACKGROUND:
		case AllocKind::OBJECT12:
		case AllocKind::OBJECT12_BACKGROUND:
		case AllocKind::OBJECT16:
		case AllocKind::OBJECT16_BACKGROUND:
		case AllocKind::FUNCTION:
		case AllocKind::FUNCTION_EXTENDED:
			return JS::TraceKind::Object;
		case AllocKind::SCRIPT:
			return JS::TraceKind::Script;
		case AllocKind::LAZY_SCRIPT:
			return JS::TraceKind::LazyScript;
		case AllocKind::SHAPE:
		case AllocKind::ACCESSOR_SHAPE:
			return JS::TraceKind::Shape;
		case AllocKind::BASE_SHAPE:
			return JS::TraceKind::BaseShape;
		case AllocKind::OBJECT_GROUP:
			return JS::TraceKind::ObjectGroup;
		case AllocKind::FAT_INLINE_STRING:
                case AllocKind::FAT_INLINE_ATOM:
		case AllocKind::STRING:
		case AllocKind::EXTERNAL_STRING:
                case AllocKind::ATOM:
			return JS::TraceKind::String;
		case AllocKind::SYMBOL:
			return JS::TraceKind::Symbol;
		case AllocKind::JITCODE:
			return JS::TraceKind::JitCode;
		case AllocKind::SCOPE:
			return JS::TraceKind::Scope;
                case AllocKind::REGEXP_SHARED:
                        return JS::TraceKind::RegExpShared;
		default:
			return JS::TraceKind::Null;
	}
}

/* static */ MOZ_ALWAYS_INLINE bool
Cell::needWriteBarrierPre(JS::Zone* zone) {
    return false;
}

/* static */ MOZ_ALWAYS_INLINE TenuredCell*
TenuredCell::fromPointer(void* ptr)
{
    return static_cast<TenuredCell*>(ptr);
}

/* static */ MOZ_ALWAYS_INLINE const TenuredCell*
TenuredCell::fromPointer(const void* ptr)
{
    return static_cast<const TenuredCell*>(ptr);
}

bool
TenuredCell::isMarkedAny() const
{
	return IsMarkedCell(this);
}

bool
TenuredCell::isMarkedBlack() const
{
	return IsMarkedCell(this);
}

bool
TenuredCell::isMarkedGray() const
{
    return false;
}

bool
TenuredCell::markIfUnmarked(MarkColor color /* = Black */) const
{
	return true;
}

void
TenuredCell::markBlack() const
{
	MOZ_ASSERT(false);
}

void
TenuredCell::copyMarkBitsFrom(const TenuredCell* src)
{
}

JS::TraceKind
TenuredCell::getTraceKind() const
{
    return Cell::getTraceKind();
}

/* static */ MOZ_ALWAYS_INLINE void
TenuredCell::readBarrier(TenuredCell* thing)
{
}

void
AssertSafeToSkipBarrier(TenuredCell* thing);

/* static */ MOZ_ALWAYS_INLINE void
TenuredCell::writeBarrierPre(TenuredCell* thing)
{
}

static MOZ_ALWAYS_INLINE void
AssertValidToSkipBarrier(TenuredCell* thing)
{
}
/* static */ MOZ_ALWAYS_INLINE void
TenuredCell::writeBarrierPost(void* cellp, TenuredCell* prior, TenuredCell* next)
{
    // OMR Writebarriers
    standardWriteBarrier(omrjs::omrVMThread, (omrobjectptr_t)cellp, (omrobjectptr_t)next);
}

#ifdef DEBUG
bool
Cell::isAligned() const
{
    return true;
}

bool
TenuredCell::isAligned() const
{
    return true;
}
#endif // DEBUG

} /* namespace gc */

namespace debug {

// Utility functions meant to be called from an interactive debugger.
enum class MarkInfo : int {
    BLACK = 0,
    GRAY = 1,
    UNMARKED = -1,
    NURSERY = -2,
};

// Get the mark color for a cell, in a way easily usable from a debugger.
MOZ_NEVER_INLINE MarkInfo
GetMarkInfo(js::gc::Cell* cell);

// Sample usage from gdb:
//
//   (gdb) p $word = js::debug::GetMarkWordAddress(obj)
//   $1 = (uintptr_t *) 0x7fa56d5fe360
//   (gdb) p/x $mask = js::debug::GetMarkMask(obj, js::gc::GRAY)
//   $2 = 0x200000000
//   (gdb) watch *$word
//   Hardware watchpoint 7: *$word
//   (gdb) cond 7 *$word & $mask
//   (gdb) cont
//
// Note that this is *not* a watchpoint on a single bit. It is a watchpoint on
// the whole word, which will trigger whenever the word changes and the
// selected bit is set after the change.
//
// So if the bit changing is the desired one, this is exactly what you want.
// But if a different bit changes (either set or cleared), you may still stop
// execution if the $mask bit happened to already be set. gdb does not expose
// enough information to restrict the watchpoint to just a single bit.

// Return the address of the word containing the mark bits for the given cell,
// or nullptr if the cell is in the nursery.
MOZ_NEVER_INLINE uintptr_t*
GetMarkWordAddress(js::gc::Cell* cell);

// Return the mask for the given cell and color bit, or 0 if the cell is in the
// nursery.
MOZ_NEVER_INLINE uintptr_t
GetMarkMask(js::gc::Cell* cell, uint32_t colorBit);

} /* namespace debug */
} /* namespace js */

#endif /* gc_Heap_h */
