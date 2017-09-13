/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/ZoneGroup.h"

#include "jscntxt.h"

#include "jit/IonBuilder.h"
#include "jit/JitCompartment.h"

using namespace js;

namespace js {

ZoneGroup::ZoneGroup(JSRuntime* runtime)
  : runtime(runtime),
    ownerContext_(TlsContext.get()),
    enterCount(1),
    zones_(this),
    usedByHelperThread(false),
#ifdef DEBUG
    ionBailAfter_(this, 0),
#endif
    jitZoneGroup(this, nullptr),
{}

bool
ZoneGroup::init()
{
    AutoLockGC lock(runtime);

    jitZoneGroup = js_new<jit::JitZoneGroup>(this);
    if (!jitZoneGroup)
        return false;

    return true;
}

ZoneGroup::~ZoneGroup()
{
    js_delete(jitZoneGroup.ref());

    if (this == runtime->gc.systemZoneGroup)
        runtime->gc.systemZoneGroup = nullptr;
}

void
ZoneGroup::enter(JSContext* cx)
{
    if (ownerContext().context() == cx) {
        MOZ_ASSERT(enterCount);
    } else {
        if (useExclusiveLocking) {
            MOZ_ASSERT(!usedByHelperThread);
            while (ownerContext().context() != nullptr) {
                cx->yieldToEmbedding();
            }
        }
        MOZ_RELEASE_ASSERT(ownerContext().context() == nullptr);
        MOZ_ASSERT(enterCount == 0);
        ownerContext_ = CooperatingContext(cx);
        if (cx->generationalDisabled)
            nursery().disable();
        // Finish any Ion compilations in this zone group, in case compilation
        // finished for some script in this group while no thread was in this
        // group.
        jit::AttachFinishedCompilations(this, nullptr);
    }
    enterCount++;
}

void
ZoneGroup::leave()
{
    MOZ_ASSERT(ownedByCurrentThread());
    MOZ_ASSERT(enterCount);
    if (--enterCount == 0)
        ownerContext_ = CooperatingContext(nullptr);
}

bool
ZoneGroup::ownedByCurrentThread()
{
    MOZ_ASSERT(TlsContext.get());
    return ownerContext().context() == TlsContext.get();
}

} // namespace js

