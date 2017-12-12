/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/DebugOnly.h"

#include "jscompartment.h"
#include "jsgc.h"

#include "gc/GCInternals.h"
#include "js/HashTable.h"
#include "vm/Runtime.h"

#include "jscntxtinlines.h"
#include "jsgcinlines.h"

using namespace js;
using namespace js::gc;

#ifndef USE_OMR // OMRTODO

void
js::IterateHeapUnbarriered(JSContext* cx, void* data,
                           IterateZoneCallback zoneCallback,
                           JSIterateCompartmentCallback compartmentCallback,
                           IterateCellCallback cellCallback)
{
}

void
js::IterateHeapUnbarrieredForZone(JSContext* cx, Zone* zone, void* data,
                                  IterateZoneCallback zoneCallback,
                                  JSIterateCompartmentCallback compartmentCallback,
                                  IterateCellCallback cellCallback)
{
}
#endif

void
js::IterateScripts(JSContext* cx, JSCompartment* compartment,
                   void* data, IterateScriptCallback scriptCallback)
{
    MOZ_ASSERT(!cx->suppressGC);
    AutoEmptyNursery empty(cx);
    AutoPrepareForTracing prep(cx, SkipAtoms);

    if (compartment) {
        Zone* zone = compartment->zone();
        for (auto script = zone->cellIter<JSScript>(empty); !script.done(); script.next()) {
            if (script->compartment() == compartment)
                scriptCallback(cx->runtime(), data, script);
        }
    } else {
        for (ZonesIter zone(cx->runtime(), SkipAtoms); !zone.done(); zone.next()) {
            for (auto script = zone->cellIter<JSScript>(empty); !script.done(); script.next())
                scriptCallback(cx->runtime(), data, script);
        }
    }
}
