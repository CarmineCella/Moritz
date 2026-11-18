// rng.h — portable random numbers: the same draws on every system and standard library.
// Self-contained (the standard library only), so Moritz builds the same engines.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// The generators of <random> (mt19937, mt19937_64) are specified to the bit, but the
// distributions (uniform_real_distribution, uniform_int_distribution, normal_distribution) are
// not: libstdc++, libc++ and MSVC give different numbers for the same seed. Everything random in
// Musil's libraries and engines draws through these functions instead, from the raw output of
// the generator:
//   uniform01(g)     a double in [0, 1) with 53 random bits (one 64-bit draw, or two 32-bit ones)
//   uniform(g, a, b) a double in [a, b)
//   below(g, n)      an integer in [0, n), unbiased (Lemire's multiply-and-reject on 32 bits)
//   between(g, a, b) an integer in [a, b], both included
//   gaussian(g)      a standard normal deviate (Box-Muller from two uniform01 draws)
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace musil {
namespace rnd {

// a 32-bit draw from a generator of 32 or 64 bits (the high half of a 64-bit draw)
template <class G>
inline uint32_t bits32(G& g) {
    if constexpr (G::max() > 0xffffffffull) return (uint32_t)((uint64_t)g() >> 32);
    else return (uint32_t)g();
}
// [0, 1) with 53 random bits
template <class G>
inline double uniform01(G& g) {
    if constexpr (G::max() > 0xffffffffull) {
        return (double)((uint64_t)g() >> 11) * (1.0 / 9007199254740992.0);
    } else {
        uint64_t a = (uint32_t)g() >> 5, b = (uint32_t)g() >> 6;      // 27 + 26 bits
        return (double)(a * 67108864ull + b) * (1.0 / 9007199254740992.0);
    }
}
template <class G>
inline double uniform(G& g, double a, double b) { return a + (b - a) * uniform01(g); }
// [0, n), n >= 1 (n 0 gives 0); unbiased for any n < 2^32
template <class G>
inline uint32_t below(G& g, uint32_t n) {
    if (n <= 1) return 0;
    uint64_t m = (uint64_t)bits32(g) * n;
    uint32_t low = (uint32_t)m;
    if (low < n) {
        uint32_t t = (uint32_t)(-n) % n;
        while (low < t) { m = (uint64_t)bits32(g) * n; low = (uint32_t)m; }
    }
    return (uint32_t)(m >> 32);
}
// [a, b], a <= b
template <class G>
inline long between(G& g, long a, long b) { return b <= a ? a : a + (long)below(g, (uint32_t)(b - a + 1)); }
// a standard normal deviate
template <class G>
inline double gaussian(G& g) {
    double u = uniform01(g), v = uniform01(g);
    return std::sqrt(-2.0 * std::log(1.0 - u)) * std::cos(6.283185307179586 * v);
}

}  // namespace rnd
}  // namespace musil
