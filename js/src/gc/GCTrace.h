/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCTrace_h
#define gc_GCTrace_h

#include "gc/Heap.h"

namespace js {

class ObjectGroup;

namespace gc {

#ifdef JS_GC_TRACE

bool TraceEnabled() { return false; }
void TraceCreateObject(JSObject* object) {}
void TraceTypeNewScript(js::ObjectGroup* group) {}

#else

inline bool TraceEnabled() { return false; }
inline void TraceCreateObject(JSObject* object) {}
inline void TraceTypeNewScript(js::ObjectGroup* group) {}

#endif

} /* namespace gc */
} /* namespace js */

#endif
