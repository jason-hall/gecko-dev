/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Nursery-inl.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/Move.h"
#include "mozilla/Unused.h"

#include "jscompartment.h"
#include "jscntxt.h"
#include "jsfriendapi.h"
#include "jsgc.h"
#include "jsutil.h"

#include "gc/GCInternals.h"
#include "gc/Memory.h"
#include "jit/JitFrames.h"
#include "vm/ArrayObject.h"
#include "vm/Debugger.h"
#if defined(DEBUG)
#include "vm/EnvironmentObject.h"
#endif
#include "vm/Time.h"
#include "vm/TypedArrayObject.h"
#include "vm/TypeInference.h"

#include "jsobjinlines.h"

#include "vm/NativeObject-inl.h"

#include "omrgc.h"

using namespace js;
using namespace gc;

using mozilla::ArrayLength;
using mozilla::DebugOnly;
using mozilla::PodCopy;
using mozilla::PodZero;


OMR_VMThread* js::Nursery::omrVMThread = nullptr;
OMR_VM* js::Nursery::omrVM = nullptr;

void
js::Nursery::disable()
{

}

JSObject*
js::Nursery::allocateObject(JSContext* cx, size_t size, size_t numDynamic, const js::Class* clasp, bool canGC)
{
	JSObject* obj = nullptr;
	if (canGC) {
		obj = (JSObject *)OMR_GC_Allocate(Nursery::omrVMThread, 0, size, 0);
	} else {
		obj = (JSObject *)OMR_GC_AllocateNoGC(Nursery::omrVMThread, 0, size, 0);
	}
	if (obj) {
		if (numDynamic > 0) {
			HeapSlot* slots = (HeapSlot *)malloc(numDynamic * sizeof(HeapSlot *));//OMR_GC_AllocateNoGC(Nursery::omrVMThread, 0, numDynamic * sizeof(HeapSlot *), 0);
			if (!slots)
				return nullptr;
			obj->setInitialSlotsMaybeNonNative(slots);
		}
		else
			obj->setInitialSlotsMaybeNonNative(nullptr);
	}
	return obj;
}
