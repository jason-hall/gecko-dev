/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Memory_h
#define gc_Memory_h

#include <stddef.h>
#include <unistd.h>

namespace js {
namespace gc {

inline size_t SystemPageSize() { return getpagesize(); }

// Allocate memory mapped content.
// The offset must be aligned according to alignment requirement.
inline void* AllocateMappedContent(int fd, size_t offset, size_t length, size_t alignment) { return nullptr; }

inline void MakePagesReadOnly(void* p, size_t size) {}
inline void UnprotectPages(void* p, size_t size) {}

inline void DeallocateMappedContent(void* p, size_t length) {}

} // namespace gc
} // namespace js

#endif /* gc_Memory_h */
