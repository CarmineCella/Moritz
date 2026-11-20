// edit.h — editing sampled sound: gain, fades, peak normalisation, reversal and channel conversion, on
// interleaved frames (a Musil vector is one channel: ch = 1). Musil's fade-in, fade-out and normalize-peak, and
// the edits of Moritz's sound editor. Self-contained (the standard library).
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// x holds frames * ch samples, frame after frame. The fades are linear over n frames, as Musil's always were:
// a fade in multiplies frame k by k / n (the first frame is silent, the last just below full), a fade out is the
// fade in backwards (frame k by (n - 1 - k) / n: the last frame is silent).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace musil {
namespace dsp {
namespace edit {

template <class T>
inline void gain(T* x, size_t frames, int ch, double g) {
    for (size_t k = 0, n = frames * (size_t)ch; k < n; ++k) x[k] = (T)(x[k] * g);
}

inline double db_to_amp(double db) { return std::pow(10.0, db / 20.0); }

// the first n frames of x (n <= frames) faded in
template <class T>
inline void fade_in(T* x, size_t frames, int ch, size_t n) {
    n = std::min(n, frames);
    for (size_t f = 0; f < n; ++f) {
        double g = (double)f / (double)n;
        for (int c = 0; c < ch; ++c) x[f * (size_t)ch + (size_t)c] = (T)(x[f * (size_t)ch + (size_t)c] * g);
    }
}

// the last n frames of x faded out
template <class T>
inline void fade_out(T* x, size_t frames, int ch, size_t n) {
    n = std::min(n, frames);
    size_t from = frames - n;
    for (size_t f = 0; f < n; ++f) {
        double g = (double)(n - 1 - f) / (double)n;
        for (int c = 0; c < ch; ++c) {
            T& v = x[(from + f) * (size_t)ch + (size_t)c];
            v = (T)(v * g);
        }
    }
}

// the largest absolute sample (0 for silence or nothing)
template <class T>
inline double peak(const T* x, size_t frames, int ch) {
    double p = 0;
    for (size_t k = 0, n = frames * (size_t)ch; k < n; ++k) p = std::max(p, (double)std::fabs((double)x[k]));
    return p;
}

// scaled so the peak is target (each sample divided by the peak, then times target: Musil's normalize-peak for
// target 1); silence stays as it is. Returns the gain applied (1 for silence).
template <class T>
inline double normalize_peak(T* x, size_t frames, int ch, double target = 1.0) {
    double p = peak(x, frames, ch);
    if (p == 0) return 1.0;
    for (size_t k = 0, n = frames * (size_t)ch; k < n; ++k) x[k] = (T)((double)x[k] / p * target);
    return target / p;
}

// the frames in reverse order (each frame's channels stay in theirs)
template <class T>
inline void reverse(T* x, size_t frames, int ch) {
    if (frames < 2) return;
    for (size_t a = 0, b = frames - 1; a < b; ++a, --b)
        for (int c = 0; c < ch; ++c) std::swap(x[a * (size_t)ch + (size_t)c], x[b * (size_t)ch + (size_t)c]);
}

// frames of src (srcCh channels) as dstCh channels: the same when they agree; a mono source goes to every
// channel; to fewer channels, channel c is the mean of the source channels k with k % dstCh == c (to mono:
// the mean of all); to more, channel c takes source channel c % srcCh
template <class T>
inline std::vector<T> convert_channels(const T* src, size_t frames, int srcCh, int dstCh) {
    std::vector<T> out(frames * (size_t)std::max(0, dstCh));
    if (srcCh <= 0 || dstCh <= 0) return out;
    for (size_t f = 0; f < frames; ++f) {
        const T* s = src + f * (size_t)srcCh;
        T* d = out.data() + f * (size_t)dstCh;
        if (srcCh == dstCh) {
            std::copy(s, s + srcCh, d);
        } else if (dstCh < srcCh) {
            for (int c = 0; c < dstCh; ++c) {
                double sum = 0;
                int count = 0;
                for (int k = c; k < srcCh; k += dstCh) { sum += (double)s[k]; ++count; }
                d[c] = (T)(sum / count);
            }
        } else {
            for (int c = 0; c < dstCh; ++c) d[c] = s[c % srcCh];
        }
    }
    return out;
}

}  // namespace edit
}  // namespace dsp
}  // namespace musil
