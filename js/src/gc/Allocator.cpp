/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Allocator.h"

#include "jscntxt.h"

#include "gc/GCInternals.h"
#include "gc/GCTrace.h"
#include "gc/Nursery.h"
#include "jit/JitCompartment.h"
#include "threading/CpuCount.h"
#include "vm/Runtime.h"
#include "vm/String.h"

#include "jsobjinlines.h"

#include "gc/Heap-inl.h"

using namespace js;
using namespace gc;

namespace js {

template <>
Shape*
Allocate<Shape, CanGC>(JSContext* cx) {
	return Allocate<Shape, CanGC>(cx, gc::AllocKind::SHAPE);
}
template <>
Shape*
Allocate<Shape, NoGC>(JSContext* cx) {
	return Allocate<Shape, NoGC>(cx, gc::AllocKind::SHAPE);
}

template <>
AccessorShape*
Allocate<AccessorShape, CanGC>(JSContext* cx) {
	return Allocate<AccessorShape, CanGC>(cx, gc::AllocKind::ACCESSOR_SHAPE);
}
template <>
AccessorShape*
Allocate<AccessorShape, NoGC>(JSContext* cx) {
	return Allocate<AccessorShape, NoGC>(cx, gc::AllocKind::ACCESSOR_SHAPE);
}

template <>
BaseShape*
Allocate<BaseShape, CanGC>(JSContext* cx) {
	return Allocate<BaseShape, CanGC>(cx, gc::AllocKind::BASE_SHAPE);
}
template <>
BaseShape*
Allocate<BaseShape, NoGC>(JSContext* cx) {
	return Allocate<BaseShape, NoGC>(cx, gc::AllocKind::BASE_SHAPE);
}

template <>
JSScript*
Allocate<JSScript, CanGC>(JSContext* cx) {
	return Allocate<JSScript, CanGC>(cx, gc::AllocKind::SCRIPT);
}
template <>
JSScript*
Allocate<JSScript, NoGC>(JSContext* cx) {
	return Allocate<JSScript, NoGC>(cx, gc::AllocKind::SCRIPT);
}

template <>
JS::Symbol*
Allocate<JS::Symbol, CanGC>(JSContext* cx) {
	return Allocate<JS::Symbol, CanGC>(cx, gc::AllocKind::SYMBOL);
}
template <>
JS::Symbol*
Allocate<JS::Symbol, NoGC>(JSContext* cx) {
	return Allocate<JS::Symbol, NoGC>(cx, gc::AllocKind::SYMBOL);
}

template <>
JSString*
Allocate<JSString, CanGC>(JSContext* cx) {
	return Allocate<JSString, CanGC>(cx, gc::AllocKind::STRING);
}
template <>
JSString*
Allocate<JSString, NoGC>(JSContext* cx) {
	return Allocate<JSString, NoGC>(cx, gc::AllocKind::STRING);
}

template <>
JSFatInlineString*
Allocate<JSFatInlineString, CanGC>(JSContext* cx) {
	return Allocate<JSFatInlineString, CanGC>(cx, gc::AllocKind::FAT_INLINE_STRING);
}
template <>
JSFatInlineString*
Allocate<JSFatInlineString, NoGC>(JSContext* cx) {
	return Allocate<JSFatInlineString, NoGC>(cx, gc::AllocKind::FAT_INLINE_STRING);
}

template <>
JSExternalString*
Allocate<JSExternalString, CanGC>(JSContext* cx) {
	return Allocate<JSExternalString, CanGC>(cx, gc::AllocKind::EXTERNAL_STRING);
}
template <>
JSExternalString*
Allocate<JSExternalString, NoGC>(JSContext* cx) {
	return Allocate<JSExternalString, NoGC>(cx, gc::AllocKind::EXTERNAL_STRING);
}

template <>
js::ObjectGroup*
Allocate<js::ObjectGroup, CanGC>(JSContext* cx) {
	return Allocate<js::ObjectGroup, CanGC>(cx, gc::AllocKind::OBJECT_GROUP);
}
template <>
js::ObjectGroup*
Allocate<js::ObjectGroup, NoGC>(JSContext* cx) {
	return Allocate<js::ObjectGroup, NoGC>(cx, gc::AllocKind::OBJECT_GROUP);
}

template <>
js::Scope*
Allocate<js::Scope, CanGC>(JSContext* cx) {
	return Allocate<js::Scope, CanGC>(cx, gc::AllocKind::SCOPE);
}
template <>
js::Scope*
Allocate<js::Scope, NoGC>(JSContext* cx) {
	return Allocate<js::Scope, NoGC>(cx, gc::AllocKind::SCOPE);
}

template <>
js::LazyScript*
Allocate<js::LazyScript, CanGC>(JSContext* cx) {
	return Allocate<js::LazyScript, CanGC>(cx, gc::AllocKind::LAZY_SCRIPT);
}
template <>
js::LazyScript*
Allocate<js::LazyScript, NoGC>(JSContext* cx) {
	return Allocate<js::LazyScript, NoGC>(cx, gc::AllocKind::LAZY_SCRIPT);
}

template <>
js::NormalAtom*
Allocate<js::NormalAtom, CanGC>(JSContext* cx) {
	return Allocate<js::NormalAtom, CanGC>(cx, gc::AllocKind::ATOM);
}
template <>
js::NormalAtom*
Allocate<js::NormalAtom, NoGC>(JSContext* cx) {
	return Allocate<js::NormalAtom, NoGC>(cx, gc::AllocKind::ATOM);
}

template <>
js::FatInlineAtom*
Allocate<js::FatInlineAtom, CanGC>(JSContext* cx) {
	return Allocate<js::FatInlineAtom, CanGC>(cx, gc::AllocKind::FAT_INLINE_ATOM);
}
template <>
js::FatInlineAtom*
Allocate<js::FatInlineAtom, NoGC>(JSContext* cx) {
	return Allocate<js::FatInlineAtom, NoGC>(cx, gc::AllocKind::FAT_INLINE_ATOM);
}

template <>
RegExpShared*
Allocate<RegExpShared, CanGC>(JSContext* cx) {
	return Allocate<RegExpShared, CanGC>(cx, gc::AllocKind::REGEXP_SHARED);
}
template <>
RegExpShared*
Allocate<RegExpShared, NoGC>(JSContext* cx) {
	return Allocate<RegExpShared, NoGC>(cx, gc::AllocKind::REGEXP_SHARED);
}

template <typename T, AllowGC allowGC /* = CanGC */>
T*
Allocate(JSContext* cx) {
	MOZ_ASSERT(false);
	return Allocate<T, allowGC>(cx, gc::AllocKind::FIRST);
}

template <typename T, AllowGC allowGC /* = CanGC */>
T*
Allocate(JSContext* cx, gc::AllocKind kind) {
	JSRuntime* rt = cx->runtime();
	Cell* obj = rt->gc.nursery().allocateObject(cx, sizeof(T), 0, nullptr, (allowGC == CanGC) && (rt->gc.enabled == 0));
	if (obj != NULL) {
		obj->setAllocKind(kind);
	}

	return (T*)obj;
}

} // namespace js

template JSObject* js::Allocate<JSObject, NoGC>(JSContext* cx, gc::AllocKind kind,
                                                size_t nDynamicSlots, gc::InitialHeap heap,
                                                const Class* clasp);
template JSObject* js::Allocate<JSObject, CanGC>(JSContext* cx, gc::AllocKind kind,
                                                 size_t nDynamicSlots, gc::InitialHeap heap,
                                                 const Class* clasp);

template <typename T, AllowGC allowGC /* = CanGC */>
JSObject*
js::Allocate(JSContext* cx, gc::AllocKind kind, size_t nDynamicSlots, gc::InitialHeap heap,
         const Class* clasp) {
	JSRuntime* rt = cx->runtime();
	JSObject* obj = rt->gc.nursery().allocateObject(cx, OmrGcHelper::thingSize(kind), nDynamicSlots, clasp, (allowGC == CanGC) && (rt->gc.enabled == 0));
	if (obj) obj->setAllocKind(kind);
	return obj;
}

#define DECL_ALLOCATOR_INSTANCES(allocKind, traceKind, type, sizedType) \
    template type* js::Allocate<type, NoGC>(JSContext* cx);\
    template type* js::Allocate<type, CanGC>(JSContext* cx);\
    template type* js::Allocate<type, NoGC>(JSContext* cx, gc::AllocKind);\
    template type* js::Allocate<type, CanGC>(JSContext* cx, gc::AllocKind);
FOR_EACH_NONOBJECT_ALLOCKIND(DECL_ALLOCATOR_INSTANCES)
#undef DECL_ALLOCATOR_INSTANCES

template <AllowGC allowGC>
bool
GCRuntime::checkAllocatorState(JSContext* cx, AllocKind kind)
{
    return true;
}
