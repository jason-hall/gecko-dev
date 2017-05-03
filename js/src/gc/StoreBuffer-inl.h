/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_StoreBuffer_inl_h
#define gc_StoreBuffer_inl_h

#include "gc/StoreBuffer.h"

#include "gc/Heap.h"

#include "gc/Heap-inl.h"

namespace js {
namespace gc {

#ifndef OMR // Disable Arenas and write barriers

inline /* static */ size_t
ArenaCellSet::getCellIndex(const TenuredCell* cell)
{
    return 0;
}

inline /* static */ void
ArenaCellSet::getWordIndexAndMask(size_t cellIndex, size_t* wordp, uint32_t* maskp)
{
}

inline bool
ArenaCellSet::hasCell(size_t cellIndex) const
{
    return true;
}

inline void
ArenaCellSet::putCell(size_t cellIndex)
{
}

inline void
ArenaCellSet::check() const
{
}

inline void
StoreBuffer::putWholeCell(Cell* cell)
{
}

#endif // ! OMR

} // namespace gc
} // namespace js

#endif // gc_StoreBuffer_inl_h
