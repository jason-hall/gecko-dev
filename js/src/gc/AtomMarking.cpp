/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/AtomMarking-inl.h"

#include "jscompartment.h"

#include "jsgcinlines.h"
#include "gc/Heap-inl.h"

namespace js {
namespace gc {

// Atom Marking Overview
//
// Things in the atoms zone (which includes atomized strings and other things,
// all of which we will refer to as 'atoms' here) may be pointed to freely by
// things in other zones. To avoid the need to perform garbage collections of
// the entire runtime to collect atoms, we compute a separate atom mark bitmap
// for each zone that is always an overapproximation of the atoms that zone is
// using. When an atom is not in the mark bitmap for any zone, it can be
// destroyed.
//
// To minimize interference with the rest of the GC, atom marking and sweeping
// is done by manipulating the mark bitmaps in the chunks used for the atoms.
// When the atoms zone is being collected, the mark bitmaps for the chunk(s)
// used by the atoms are updated normally during marking. After marking
// finishes, the chunk mark bitmaps are translated to a more efficient atom
// mark bitmap (see below) that is stored on the zones which the GC collected
// (computeBitmapFromChunkMarkBits). Before sweeping begins, the chunk mark
// bitmaps are updated with any atoms that might be referenced by zones which
// weren't collected (updateChunkMarkBits). The GC sweeping will then release
// all atoms which are not marked by any zone.
//
// The representation of atom mark bitmaps is as follows:
//
// Each arena in the atoms zone has an atomBitmapStart() value indicating the
// word index into the bitmap of the first thing in the arena. Each arena uses
// ArenaBitmapWords of data to store its bitmap, which uses the same
// representation as chunk mark bitmaps: one bit is allocated per Cell, with
// bits for space between things being unused when things are larger than a
// single Cell.

template <typename T>
void
AtomMarkingRuntime::markAtom(JSContext* cx, T* thing)
{
    return inlinedMarkAtom(cx, thing);
}

template void AtomMarkingRuntime::markAtom(JSContext* cx, JSAtom* thing);
template void AtomMarkingRuntime::markAtom(JSContext* cx, JS::Symbol* thing);

void
AtomMarkingRuntime::markId(JSContext* cx, jsid id)
{
    if (JSID_IS_ATOM(id)) {
        markAtom(cx, JSID_TO_ATOM(id));
        return;
    }
    if (JSID_IS_SYMBOL(id)) {
        markAtom(cx, JSID_TO_SYMBOL(id));
        return;
    }
    MOZ_ASSERT(!JSID_IS_GCTHING(id));
}

void
AtomMarkingRuntime::markAtomValue(JSContext* cx, const Value& value)
{
    if (value.isString()) {
        if (value.toString()->isAtom())
            markAtom(cx, &value.toString()->asAtom());
        return;
    }
    if (value.isSymbol()) {
        markAtom(cx, value.toSymbol());
        return;
    }
    MOZ_ASSERT_IF(value.isGCThing(), value.isObject() || value.isPrivateGCThing());
}

void
AtomMarkingRuntime::adoptMarkedAtoms(Zone* target, Zone* source)
{
    MOZ_ASSERT(target->runtimeFromAnyThread()->currentThreadHasExclusiveAccess());
    target->markedAtoms().bitwiseOrWith(source->markedAtoms());
}

#ifdef DEBUG
template <typename T>
bool
AtomMarkingRuntime::atomIsMarked(Zone* zone, T* thing)
{
    static_assert(mozilla::IsSame<T, JSAtom>::value ||
                  mozilla::IsSame<T, JS::Symbol>::value,
                  "Should only be called with JSAtom* or JS::Symbol* argument");

    if (!zone->runtimeFromAnyThread()->permanentAtoms)
        return true;

    if (ThingIsPermanent(thing))
        return true;

    if (mozilla::IsSame<T, JSAtom>::value) {
        JSAtom* atom = reinterpret_cast<JSAtom*>(thing);
        if (AtomIsPinnedInRuntime(zone->runtimeFromAnyThread(), atom))
            return true;
    }

    size_t bit = GetAtomBit(thing);
    return zone->markedAtoms().getBit(bit);
}

template bool AtomMarkingRuntime::atomIsMarked(Zone* zone, JSAtom* thing);
template bool AtomMarkingRuntime::atomIsMarked(Zone* zone, JS::Symbol* thing);

template<>
bool
AtomMarkingRuntime::atomIsMarked(Zone* zone, TenuredCell* thing)
{
    if (!thing)
        return true;

    JS::TraceKind kind = thing->getTraceKind();
    if (kind == JS::TraceKind::String) {
        JSString* str = static_cast<JSString*>(thing);
        if (str->isAtom())
            return atomIsMarked(zone, &str->asAtom());
        return true;
    }
    if (kind == JS::TraceKind::Symbol)
        return atomIsMarked(zone, static_cast<JS::Symbol*>(thing));
    return true;
}

bool
AtomMarkingRuntime::idIsMarked(Zone* zone, jsid id)
{
    if (JSID_IS_ATOM(id))
        return atomIsMarked(zone, JSID_TO_ATOM(id));

    if (JSID_IS_SYMBOL(id))
        return atomIsMarked(zone, JSID_TO_SYMBOL(id));

    MOZ_ASSERT(!JSID_IS_GCTHING(id));
    return true;
}

bool
AtomMarkingRuntime::valueIsMarked(Zone* zone, const Value& value)
{
    if (value.isString()) {
        if (value.toString()->isAtom())
            return atomIsMarked(zone, &value.toString()->asAtom());
        return true;
    }

    if (value.isSymbol())
        return atomIsMarked(zone, value.toSymbol());

    MOZ_ASSERT_IF(value.isGCThing(), value.isObject() || value.isPrivateGCThing());
    return true;
}

#endif // DEBUG

} // namespace gc

#ifdef DEBUG

bool
AtomIsMarked(Zone* zone, JSAtom* atom)
{
    return zone->runtimeFromAnyThread()->gc.atomMarking.atomIsMarked(zone, atom);
}

bool
AtomIsMarked(Zone* zone, jsid id)
{
    return zone->runtimeFromAnyThread()->gc.atomMarking.idIsMarked(zone, id);
}

bool
AtomIsMarked(Zone* zone, const Value& value)
{
    return zone->runtimeFromAnyThread()->gc.atomMarking.valueIsMarked(zone, value);
}

#endif // DEBUG

} // namespace js
