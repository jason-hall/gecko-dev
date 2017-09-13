/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_StoreBuffer_h
#define gc_StoreBuffer_h

#include "mozilla/Attributes.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/ReentrancyGuard.h"

#include <algorithm>

#include "jsalloc.h"

#include "ds/LifoAlloc.h"
#include "gc/Nursery.h"
#include "js/MemoryMetrics.h"

namespace js {
namespace gc {

// class ArenaCellSet;

/*
 * BufferableRef represents an abstract reference for use in the generational
 * GC's remembered set. Entries in the store buffer that cannot be represented
 * with the simple pointer-to-a-pointer scheme must derive from this class and
 * use the generic store buffer interface.
 *
 * A single BufferableRef entry in the generic buffer can represent many entries
 * in the remembered set.  For example js::OrderedHashTableRef represents all
 * the incoming edges corresponding to keys in an ordered hash table.
 */
class BufferableRef
{
  public:
    virtual void trace(JSTracer* trc) = 0;
    bool maybeInRememberedSet(const Nursery&) const { return true; }
};

} /* namespace gc */
} /* namespace js */

#endif /* gc_StoreBuffer_h */
