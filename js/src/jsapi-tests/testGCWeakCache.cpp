/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
* vim: set ts=8 sts=4 et sw=4 tw=99:
*/
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsapi-tests/tests.h"

BEGIN_TEST(testWeakCacheSet)
{
    return true;
}
END_TEST(testWeakCacheSet)

BEGIN_TEST(testWeakCacheMap)
{
    return true;
}
END_TEST(testWeakCacheMap)

// Exercise WeakCache<GCVector>.
BEGIN_TEST(testWeakCacheGCVector)
{
    // Create two objects tenured and two in the nursery. If zeal is on,
    // this may fail and we'll get more tenured objects. That's fine:
    // the test will continue to work, it will just not test as much.
    JS::RootedObject tenured1(cx, JS_NewPlainObject(cx));
    JS::RootedObject tenured2(cx, JS_NewPlainObject(cx));
    JS_GC(cx);
    JS::RootedObject nursery1(cx, JS_NewPlainObject(cx));
    JS::RootedObject nursery2(cx, JS_NewPlainObject(cx));

    using ObjectVector = js::GCVector<JS::Heap<JSObject*>>;
    using Cache = JS::WeakCache<ObjectVector>;
    auto cache = Cache(JS::GetObjectZone(tenured1), ObjectVector(cx));

    CHECK(cache.append(tenured1));
    CHECK(cache.append(tenured2));
    CHECK(cache.append(nursery1));
    CHECK(cache.append(nursery2));

    JS_GC(cx);
    CHECK(cache.get().length() == 4);
    CHECK(cache.get()[0] == tenured1);
    CHECK(cache.get()[1] == tenured2);
    CHECK(cache.get()[2] == nursery1);
    CHECK(cache.get()[3] == nursery2);

    tenured2 = nursery2 = nullptr;
    JS_GC(cx);
    CHECK(cache.get().length() == 2);
    CHECK(cache.get()[0] == tenured1);
    CHECK(cache.get()[1] == nursery1);

    return true;
}
END_TEST(testWeakCacheGCVector)
