// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// alloc_guard.cpp — the allocation guard: operator new and delete replaced in the engine tests,
// counting what is allocated or freed inside Patch::process (moritz::inAudioProcess). The audio
// thread must do neither. Not under the sanitizers, whose runtimes own operator new.

#include "moritz.h"
#include "rig.h"

#include <atomic>
#include <cstdlib>
#include <new>

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define ETEST_NO_GUARD 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ETEST_NO_GUARD 1
#endif

namespace {
std::atomic<long> gAudioAllocs{0};
}

namespace etest {
long audioAllocations() { return gAudioAllocs.load(); }
#ifdef ETEST_NO_GUARD
bool allocGuardActive() { return false; }
#else
bool allocGuardActive() { return true; }
#endif
}  // namespace etest

#ifndef ETEST_NO_GUARD
namespace {
void* take(std::size_t n) {
    if (moritz::inAudioProcess()) gAudioAllocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void give(void* p) {
    if (!p) return;
    if (moritz::inAudioProcess()) gAudioAllocs.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}
}  // namespace

void* operator new(std::size_t n) { return take(n); }
void* operator new[](std::size_t n) { return take(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return take(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return take(n); } catch (...) { return nullptr; }
}
void operator delete(void* p) noexcept { give(p); }
void operator delete[](void* p) noexcept { give(p); }
void operator delete(void* p, std::size_t) noexcept { give(p); }
void operator delete[](void* p, std::size_t) noexcept { give(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { give(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { give(p); }
#endif
