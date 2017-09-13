/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Statistics.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/IntegerRange.h"
#include "mozilla/PodOperations.h"
#include "mozilla/Sprintf.h"
#include "mozilla/TimeStamp.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>

#include "jsprf.h"
#include "jsutil.h"

#include "gc/Memory.h"
#include "vm/Debugger.h"
#include "vm/HelperThreads.h"
#include "vm/Runtime.h"
#include "vm/Time.h"

using namespace js;
using namespace js::gc;
using namespace js::gcstats;

using mozilla::DebugOnly;
using mozilla::EnumeratedArray;
using mozilla::IntegerRange;
using mozilla::PodArrayZero;
using mozilla::PodZero;
using mozilla::TimeStamp;
using mozilla::TimeDuration;


JS_PUBLIC_API(const char*)
JS::gcreason::ExplainReason(JS::gcreason::Reason reason)
{
	return " ";
}

