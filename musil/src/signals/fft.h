// fft.h — the FFTs of Musil's signal engines (vocoder.h, convolution.h, mir.h ...).
// Self-contained: only the standard library, so Moritz builds the same engines into its objects.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <vector>

namespace musil {
namespace dsp {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

inline size_t next_pow2(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }

// In-place iterative radix-2 complex FFT on split arrays (re, im) of n points, n a power of two,
// with the bit reversal and the twiddles tabulated once, the twiddles of every stage contiguous
// (stage len reads len/2 consecutive values, computed directly in double precision).
// inverse() is unscaled, so inverse(forward(x)) == n * x. T is float (the real-time objects) or
// double (Musil).
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
        // stage len (2, 4, ... n) starts at len/2 - 1: 1 + 2 + ... + n/2 = n - 1 values
        cos_.assign((size_t)std::max(1, n - 1), (T)0);
        sin_.assign((size_t)std::max(1, n - 1), (T)0);
        for (int len = 2; len <= n; len <<= 1) {
            int half = len >> 1;
            for (int k = 0; k < half; ++k) {
                cos_[(size_t)(half - 1 + k)] = (T)std::cos(kTwoPi * k / len);
                sin_[(size_t)(half - 1 + k)] = (T)std::sin(kTwoPi * k / len);
            }
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
        for (int i = 0; i + 1 < n; i += 2) {                     // the first stage: no multiplications
            T ar = re[i], ai = im[i], br = re[i + 1], bi = im[i + 1];
            re[i] = ar + br; im[i] = ai + bi;
            re[i + 1] = ar - br; im[i + 1] = ai - bi;
        }
        for (int len = 4; len <= n; len <<= 1) {
            int half = len >> 1;
            const T* wc = cos_.data() + (half - 1);
            const T* ws = sin_.data() + (half - 1);
            for (int i = 0; i < n; i += len) {
                T* ar = re + i; T* ai = im + i;
                T* br = re + i + half; T* bi = im + i + half;
                for (int k = 0; k < half; ++k) {
                    T wr = wc[k], wi = sign * ws[k];
                    T tr = br[k] * wr - bi[k] * wi, ti = br[k] * wi + bi[k] * wr;
                    br[k] = ar[k] - tr; bi[k] = ai[k] - ti;
                    ar[k] += tr; ai[k] += ti;
                }
            }
        }
    }
    int n_ = 0;
    std::vector<int> rev_;
    std::vector<T> cos_, sin_;
};

// The FFT of a real signal of n points (n a power of two, >= 2) by a complex FFT of n/2 points and a
// post-twiddle: about twice as fast as the complex FFT of the same signal. forward() gives the n/2 + 1
// bins 0..n/2 (the others are their conjugates); inverse() takes them back to n samples, unscaled like
// FFT<T>: inverse(forward(x)) == n * x.
template <class T>
class RealFFT {
public:
    void init(int n) {
        if (n == n_) return;
        n_ = n;
        int h = std::max(1, n / 2);
        half_.init(h);
        zr_.assign((size_t)h, (T)0);
        zi_.assign((size_t)h, (T)0);
        wr_.resize((size_t)h / 2 + 1);
        wi_.resize((size_t)h / 2 + 1);
        for (int k = 0; k <= h / 2; ++k) {
            wr_[(size_t)k] = (T)std::cos(kTwoPi * k / n);
            wi_[(size_t)k] = (T)-std::sin(kTwoPi * k / n);
        }
    }
    int size() const { return n_; }
    // x: n samples; re, im: n/2 + 1 values each
    void forward(const T* x, T* re, T* im) {
        const int h = n_ / 2;
        for (int m = 0; m < h; ++m) { zr_[(size_t)m] = x[2 * m]; zi_[(size_t)m] = x[2 * m + 1]; }
        half_.forward(zr_.data(), zi_.data());
        re[0] = zr_[0] + zi_[0]; im[0] = 0;
        re[h] = zr_[0] - zi_[0]; im[h] = 0;
        for (int k = 1; k <= h / 2; ++k) {
            int j = h - k;
            // E = (Z[k] + conj Z[j]) / 2, O = (Z[k] - conj Z[j]) / 2i; X[k] = E + W^k O, X[j] = conj(E) - conj(W^k O)...
            T ar = zr_[(size_t)k], ai = zi_[(size_t)k], br = zr_[(size_t)j], bi = zi_[(size_t)j];
            T er = (T)0.5 * (ar + br), ei = (T)0.5 * (ai - bi);
            T or_ = (T)0.5 * (ai + bi), oi = (T)-0.5 * (ar - br);
            T wr = wr_[(size_t)k], wi = wi_[(size_t)k];
            T tr = or_ * wr - oi * wi, ti = or_ * wi + oi * wr;
            re[k] = er + tr; im[k] = ei + ti;
            re[j] = er - tr; im[j] = -(ei - ti);
        }
    }
    // re, im: n/2 + 1 bins; x: n samples (times n)
    void inverse(const T* re, const T* im, T* x) {
        const int h = n_ / 2;
        // Z[k] = E[k] + i O[k], E[k] = (X[k] + conj X[h-k]) / 2, O[k] = (X[k] - conj X[h-k]) conj(W^k) / 2
        for (int k = 0; k <= h / 2; ++k) {
            int j = h - k;
            T ar = re[k], ai = im[k], br = re[j], bi = -im[j];          // X[k], conj X[j]
            T er = ar + br, ei = ai + bi;
            T dr = ar - br, di = ai - bi;
            T wr = wr_[(size_t)k], wi = -wi_[(size_t)k];               // conj(W^k) = e^{+2 pi i k / n}
            T or_ = dr * wr - di * wi, oi = dr * wi + di * wr;
            // Z[k] = E + i O (the factors 1/2 are folded into the unscaled convention: 2 x the half transform)
            zr_[(size_t)k] = er - oi; zi_[(size_t)k] = ei + or_;
            if (j != k && j < h) {
                // Z[j] = conj(E) + i conj(-O)... from the symmetric pair
                zr_[(size_t)j] = er + oi; zi_[(size_t)j] = -ei + or_;
            }
        }
        half_.inverse(zr_.data(), zi_.data());
        for (int m = 0; m < h; ++m) { x[2 * m] = zr_[(size_t)m]; x[2 * m + 1] = zi_[(size_t)m]; }
    }

private:
    int n_ = 0;
    FFT<T> half_;
    std::vector<T> zr_, zi_, wr_, wi_;
};

// In-place iterative radix-2 FFT on interleaved (re, im) pairs, n a power of two; sign -1 forward,
// +1 inverse (unscaled). One-off transforms (no tables, no allocation: the live engine calls it on the
// audio thread); for repeated ones of a size, FFT<T> or RealFFT<T>.
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

// The plans of the offline code, one per size and thread (the tables are built once): never on an
// audio thread (the first request of a size allocates).
template <class T>
inline FFT<T>& fft_plan(int n) {
    thread_local std::map<int, std::unique_ptr<FFT<T>>> plans;
    auto& p = plans[n];
    if (!p) { p.reset(new FFT<T>()); p->init(n); }
    return *p;
}
template <class T>
inline RealFFT<T>& rfft_plan(int n) {
    thread_local std::map<int, std::unique_ptr<RealFFT<T>>> plans;
    auto& p = plans[n];
    if (!p) { p.reset(new RealFFT<T>()); p->init(n); }
    return *p;
}

}  // namespace dsp
}  // namespace musil
