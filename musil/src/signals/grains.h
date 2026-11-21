// grains.h — the grain core shared by Musil's granular engines: Nebula (nebula.h: Musil's nebula, Moritz's
// nebula~) and Psola (psola.h: Musil's harmonize, Moritz's harmonize~). Self-contained (the standard library only).
//
// Copyright (c) 2011-2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
//   GrainWindow   a grain envelope tabulated over kSize + 1 points (hann, hamming, blackman), read with linear
//                 interpolation at a position 0 .. kSize
//   GrainPool<G>  a fixed number of grain slots, allocated once: a new grain takes the first free slot (the
//                 order the engines rely on, so a seed gives the same output), none when all are busy
//   wrapPos       a read position brought back into [0, len)
//   readTable     a guarded table (n + 1 points) read with linear interpolation
//   readWrapped   one channel of interleaved samples read with linear interpolation, wrapping at the end
// Nothing allocates after construction or set(): the real-time engines call them on the audio thread.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace musil {
namespace dsp {
namespace grains {

constexpr double kPi = 3.14159265358979323846;

// x wrapped into [0, len): a step at a time when it is near (the usual case), fmod only for a far one
inline double wrapPos(double x, double len) {
    if (x >= len) { x -= len; if (x >= len) x = std::fmod(x, len); }
    else if (x < 0) { x += len; if (x < 0) { x = std::fmod(x, len); if (x < 0) x += len; } }
    return x >= len ? 0.0 : x;
}

// a table with a guard point (t[n] = t[0]) at x in [0, n)
inline double readTable(const double* t, double x) {
    int i = (int)x;
    return t[i] + (x - i) * (t[i + 1] - t[i]);
}

// frames samples, every stride-th of data, at x in [0, frames): the last one interpolates towards the first
template <class T>
inline double readWrapped(const T* data, long frames, int stride, double x) {
    if (frames <= 0) return 0;
    if (x >= (double)frames) x = std::fmod(x, (double)frames);   // the sound was replaced by a shorter one
    long i = (long)x;
    long j = i + 1 < frames ? i + 1 : 0;
    double a = (double)data[(size_t)i * stride], c = (double)data[(size_t)j * stride];
    return a + (x - i) * (c - a);
}

class GrainWindow {
public:
    static constexpr int kSize = 4096;
    GrainWindow() { set("hann"); }
    static bool known(const std::string& type) {
        return type == "hann" || type == "hamming" || type == "hamm" || type == "blackman" || type == "black";
    }
    // hann, hamming or blackman (an unknown name is hann); allocates the first time only
    void set(const std::string& type) {
        double a0 = .5, a1 = .5, a2 = 0;
        if (type == "hamming" || type == "hamm") { a0 = .54; a1 = .46; }
        else if (type == "blackman" || type == "black") { a0 = .42; a1 = .5; a2 = .08; }
        w_.assign((size_t)kSize + 1, 0);
        for (int i = 0; i <= kSize; ++i) {
            double t = 2 * kPi * i / kSize;
            w_[(size_t)i] = a0 - a1 * std::cos(t) + a2 * std::cos(2 * t);
        }
        double s = 0, s2 = 0;
        for (int i = 0; i < kSize; ++i) { s += w_[(size_t)i]; s2 += w_[(size_t)i] * w_[(size_t)i]; }
        mean_ = s / kSize;
        meanSquare_ = s2 / kSize;
    }
    // the envelope at e in [0, kSize)
    double at(double e) const {
        int ei = (int)e;
        return w_[(size_t)ei] + (e - ei) * (w_[(size_t)ei + 1] - w_[(size_t)ei]);
    }
    double mean() const { return mean_; }               // its average (hann: 1/2)
    double meanSquare() const { return meanSquare_; }   // the average of its square (hann: 3/8)

private:
    std::vector<double> w_;
    double mean_ = .5, meanSquare_ = .375;
};

// G has a bool `active`
template <class G>
class GrainPool {
public:
    void allocate(int n) { g_.assign((size_t)std::max(0, n), G{}); }
    int size() const { return (int)g_.size(); }
    // the first free slot among the first `limit`, or -1
    int firstFree(int limit) const {
        int n = std::min(limit, (int)g_.size());
        for (int k = 0; k < n; ++k)
            if (!g_[(size_t)k].active) return k;
        return -1;
    }
    int active() const { int n = 0; for (auto& g : g_) n += g.active; return n; }
    void clear() { for (auto& g : g_) g.active = false; }
    G& operator[](int k) { return g_[(size_t)k]; }
    const G& operator[](int k) const { return g_[(size_t)k]; }
    typename std::vector<G>::iterator begin() { return g_.begin(); }
    typename std::vector<G>::iterator end() { return g_.end(); }
    typename std::vector<G>::const_iterator begin() const { return g_.begin(); }
    typename std::vector<G>::const_iterator end() const { return g_.end(); }

private:
    std::vector<G> g_;
};

}  // namespace grains
}  // namespace dsp
}  // namespace musil
