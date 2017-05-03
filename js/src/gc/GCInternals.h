/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCInternals_h
#define gc_GCInternals_h

#include "mozilla/ArrayUtils.h"
#include "mozilla/Maybe.h"
#include "mozilla/PodOperations.h"

#include "jscntxt.h"

#include "gc/Zone.h"
#include "vm/HelperThreads.h"
#include "vm/Runtime.h"

namespace js {
namespace gc {

void FinishGC(JSContext* cx);

class MOZ_RAII AutoPrepareForTracing
{
    mozilla::Maybe<AutoTraceSession> session_;

  public:
    AutoPrepareForTracing(JSContext* cx, ZoneSelector selector);
    AutoTraceSession& session() { return session_.ref(); }
};

#ifdef JSGC_HASH_TABLE_CHECKS
void CheckHeapAfterGC(JSRuntime* rt);
#endif

} /* namespace gc */
} /* namespace js */

#endif /* gc_GCInternals_h */
