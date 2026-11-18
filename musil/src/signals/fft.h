// fft.h — the FFTs of Musil's signal engines (vocoder.h, convolution.h, mir.h ...).
// Self-contained: only the standard library, so Moritz builds the same engines into its objects.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace musil {
namespace dsp {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

inline size_t next_pow2(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }

// In-place iterative radix-2 complex FFT on split arrays (re, im) of n points, n a power of two,
// with the twiddles and the bit reversal tabulated once. inverse() is unscaled, so
// inverse(forward(x)) == n * x. T is float (the real-time objects) or double (Musil).
template <class T>
class FFT {
public:
    void init(int n) {
        if (n == n_) return;
        n_ = n;
        rev_.assign((size_t)n, 0);
        for (int i = 0, j = 0; i < n; ++i) {
            rev_[(size_t)i] = j;
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j |= bit;
        }
        cos_.resize((size_t)n / 2);
        sin_.resize((size_t)n / 2);
        for (int k = 0; k < n / 2; ++k) {
            cos_[(size_t)k] = (T)std::cos(kTwoPi * k / n);
            sin_[(size_t)k] = (T)std::sin(kTwoPi * k / n);
        }
    }
    int size() const { return n_; }
    void forward(T* re, T* im) const { run(re, im, (T)-1); }
    void inverse(T* re, T* im) const { run(re, im, (T)1); }

private:
    void run(T* re, T* im, T sign) const {
        const int n = n_;
        for (int i = 0; i < n; ++i) {
            int j = rev_[(size_t)i];
            if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        for (int len = 2; len <= n; len <<= 1) {
            int half = len >> 1, step = n / len;
            for (int i = 0; i < n; i += len)
                for (int k = 0; k < half; ++k) {
                    T wr = cos_[(size_t)(k * step)], wi = sign * sin_[(size_t)(k * step)];
                    T* ar = re + i + k; T* ai = im + i + k;
                    T* br = re + i + k + half; T* bi = im + i + k + half;
                    T tr = *br * wr - *bi * wi, ti = *br * wi + *bi * wr;
                    *br = *ar - tr; *bi = *ai - ti;
                    *ar += tr; *ai += ti;
                }
        }
    }
    int n_ = 0;
    std::vector<int> rev_;
    std::vector<T> cos_, sin_;
};

// In-place iterative radix-2 FFT on interleaved (re, im) pairs, n a power of two; sign -1 forward,
// +1 inverse (unscaled). One-off transforms (no tables); for repeated ones of a size, FFT<T>.
inline void fft_inplace(double* d, size_t n, int sign) {
    for (size_t i = 1, j = 0; i < n; i++) {                       // bit reversal
        size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit;
        if (i < j) { std::swap(d[2 * i], d[2 * j]); std::swap(d[2 * i + 1], d[2 * j + 1]); }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = sign * kTwoPi / (double)len, wr = std::cos(ang), wi = std::sin(ang);
        for (size_t i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (size_t j = 0; j < len / 2; j++) {
                size_t p = 2 * (i + j), q = 2 * (i + j + len / 2);
                double ur = d[p], ui = d[p + 1], vr = d[q] * cr - d[q + 1] * ci, vi = d[q] * ci + d[q + 1] * cr;
                d[p] = ur + vr; d[p + 1] = ui + vi; d[q] = ur - vr; d[q + 1] = ui - vi;
                double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
}

}  // namespace dsp
}  // namespace musil
