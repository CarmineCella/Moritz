// denormal.h — flush-to-zero for the loops of the signal engines. Self-contained.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// A recursive filter or a reverberation tail decaying towards silence goes through denormal numbers,
// which x86 processors compute up to a hundred times slower. DenormalGuard sets flush-to-zero (and
// denormals-are-zero) for its lifetime and restores the previous mode after, so it can wrap any loop
// on any thread, an audio callback or an interpreter's builtin, without changing the caller's state:
//     { dsp::DenormalGuard ftz; ...the loop... }
// x86-64 (SSE: MXCSR FTZ | DAZ) and AArch64 (FPCR FZ); elsewhere it does nothing.
#pragma once

#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1) || defined(__SSE__)
#include <xmmintrin.h>
#define MUSIL_FTZ_SSE 1
#elif defined(__aarch64__) && !defined(_MSC_VER)
#define MUSIL_FTZ_ARM64 1
#endif

namespace musil {
namespace dsp {

class DenormalGuard {
public:
    DenormalGuard() {
#if defined(MUSIL_FTZ_SSE)
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040u);                       // FTZ (bit 15) | DAZ (bit 6)
#elif defined(MUSIL_FTZ_ARM64)
        uint64_t fpcr;
        asm volatile("mrs %0, fpcr" : "=r"(fpcr));
        saved_ = fpcr;
        asm volatile("msr fpcr, %0" ::"r"(fpcr | (1ull << 24)));   // FZ
#endif
    }
    ~DenormalGuard() {
#if defined(MUSIL_FTZ_SSE)
        _mm_setcsr(saved_);
#elif defined(MUSIL_FTZ_ARM64)
        asm volatile("msr fpcr, %0" ::"r"(saved_));
#endif
    }
    DenormalGuard(const DenormalGuard&) = delete;
    DenormalGuard& operator=(const DenormalGuard&) = delete;
    // is flush-to-zero on now (for the tests)?
    static bool active() {
#if defined(MUSIL_FTZ_SSE)
        return (_mm_getcsr() & 0x8000u) != 0;
#elif defined(MUSIL_FTZ_ARM64)
        uint64_t fpcr;
        asm volatile("mrs %0, fpcr" : "=r"(fpcr));
        return (fpcr & (1ull << 24)) != 0;
#else
        return false;
#endif
    }

private:
#if defined(MUSIL_FTZ_SSE)
    unsigned int saved_ = 0;
#else
    uint64_t saved_ = 0;
#endif
};

}  // namespace dsp
}  // namespace musil
