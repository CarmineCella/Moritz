// nebula.h — Nebula, the granular engine of sound_math: Musil's nebula renders a cloud into a
// vector, Moritz's nebula~ plays one in real time. Self-contained (the standard library only).
//
// Copyright (c) 2011-2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// Grains are windowed snippets of two prototypes, each a waveform (sine, triangle, square, noise)
// or a sound, mixed by the morph amount. A grain starts every 1 / density seconds and lasts
// `length` ms; its frequency is in Hz for a waveform and a speed ratio for a sound (1 = as
// recorded), unless the sound's pitch is given: then it is in Hz too (the speed is freq / pitch),
// so a waveform and a sound can be morphed at the same pitch. A grain can glide (gliss: towards f
// times 1 +- gliss, at random) and is placed in the stereo field at random around `jitter` (0
// right .. 1 left). The randomness of density, length, frequency and jitter is a ratio of the
// value: density 40 with randomness 0.5 gives 20 to 60 grains a second. With grainlets n, each
// grain lasts n periods of its frequency. `speed` is how fast the read position of a sound moves
// on (grains start where it is). The random numbers are Park-Miller's, as in the original, so a
// seed gives the same cloud everywhere. Triangle and square are band-limited per octave (mipmapped
// tables): a high grain reads a table with fewer harmonics, so nothing folds over the Nyquist frequency.
// A prototype with no weight in a grain (morph 0 or 1) is not read. The window, the grain slots and the
// interpolated readers are the grain core shared with the harmonizer (grains.h).
#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "grains.h"

namespace musil {
namespace dsp {

template <class T>
class Nebula {
public:
    static constexpr int kTable = 4096, kWin = grains::GrainWindow::kSize, kMaxPoly = 1024;
    static constexpr int kLevels = 6;          // band-limited tables: odd harmonics up to 63, 31, 15, 7, 3, 1

    // the parameters, read by process(); the caller changes them between calls
    struct Params {
        bool on = false;
        double density = 10, densityRnd = 0;     // grains per second; randomness 0..0.99
        double length = 100, lengthRnd = 0;      // ms; randomness 0..1
        double freq = 220, freqRnd = 0;          // Hz, or a speed ratio for a sound; randomness 0..1
        double morph = 0;                        // 0 the first prototype .. 1 the second
        double amp = 0.25;
        double gliss = 0;                        // 0..1
        double jitter = 0.5, jitterRnd = 1;      // pan 0 right .. 1 left; randomness 0..1
        double speed = 1;                        // the read position through a sound
        double grainlets = 0;                    // periods per grain (0 off)
        int poly = 256;                          // at most this many grains at once (1..kMaxPoly)
        double pitch[2] = {0, 0};                // a sound prototype's pitch in Hz (0: freq is a ratio)
    };
    Params p;

    Nebula() {
        for (int k = 0; k < 2; ++k) setWave(k, "sine");
        grains_.allocate(kMaxPoly);
    }
    static bool isWave(const std::string& w) { return w == "sine" || w == "triangle" || w == "square" || w == "noise"; }
    // prototype k (0 or 1) is a waveform; false if the name is not one. Triangle and square are band-limited
    // and mipmapped: a table per octave of their harmonics (odd ones up to 63, 31 ... 1), and each grain reads
    // the richest one whose harmonics stay under the Nyquist frequency at its frequency (and glissando)
    bool setWave(int k, const std::string& w) {
        if (!prepareWave(k, w)) return false;
        commitWave(k);
        return true;
    }
    // setWave in two halves, for a host whose audio thread runs process(): prepareWave builds the tables aside
    // (it allocates; process() never reads them, so no lock is needed), commitWave swaps them in (no allocation,
    // under whatever excludes process()); the tables it replaces wait aside until the next prepareWave frees them
    bool prepareWave(int k, const std::string& w) {
        if (!isWave(w)) return false;
        Proto& pr = next_[k];
        pr.data = nullptr;
        pr.frames = 0;
        bool limited = w == "square" || w == "triangle";
        pr.levels.assign(limited ? kLevels : 1, std::vector<double>((size_t)kTable + 1, 0.0));
        pr.maxHarm.assign(pr.levels.size(), 0);
        unsigned seed = 12345;
        double scale = 1;
        for (size_t lv = 0; lv < pr.levels.size(); ++lv) {
            int H = 63 >> lv;                                  // 63, 31, 15, 7, 3, 1
            pr.maxHarm[lv] = limited ? H : 0;
            auto& tab = pr.levels[lv];
            for (int i = 0; i < kTable; ++i) {
                double t = (double)i / kTable, v = 0;
                if (w == "sine") v = std::sin(2 * kPi_ * t);
                else if (w == "noise") { seed = seed * 1103515245u + 12345u; v = (seed >> 8) / 8388608.0 - 1.0; }
                else
                    for (int h = 1; h <= H; h += 2)            // band-limited, odd harmonics
                        v += w == "square" ? std::sin(2 * kPi_ * h * t) / h
                                           : ((h / 2) % 2 ? -1.0 : 1.0) * std::sin(2 * kPi_ * h * t) / (h * h);
                tab[(size_t)i] = v;
            }
            if (lv == 0) {                                     // the full table's peak scales every level alike
                double peak = 0;
                for (double v : tab) peak = std::max(peak, std::fabs(v));
                scale = 1.0 / std::max(peak, 1e-9);
            }
            for (auto& v : tab) v *= scale;
            tab[(size_t)kTable] = tab[0];
        }
        return true;
    }
    // prototype k is a sound: frames samples, every stride-th of data (one channel of interleaved
    // data), recorded at sr. The data must stay valid while process() runs; a real-time caller
    // sets it again before every block. frames 0: silence.
    void setSound(int k, const T* data, long frames, int stride, double sr) {
        Proto& pr = proto_[k];
        pr.levels.clear();
        pr.maxHarm.clear();
        pr.data = data;
        pr.frames = data ? std::max(0L, frames) : 0;
        pr.stride = std::max(1, stride);
        pr.sr = sr;
        if (pr.frames > 0) pr.readPos = wrap(pr.readPos, (double)pr.frames);
    }
    void commitWave(int k) {
        std::swap(proto_[k].levels, next_[k].levels);
        std::swap(proto_[k].maxHarm, next_[k].maxHarm);
        proto_[k].data = nullptr;
        proto_[k].frames = 0;
    }
    bool isSound(int k) const { return proto_[k].levels.empty(); }
    // the grain envelope: hann, hamming or blackman (an unknown name is hann)
    void setWindow(const std::string& type) { window_.set(type); }
    void seed(unsigned s) { rng_ = s ? s : 1; }
    // grains still sounding
    int active() const { return grains_.active(); }

    // n samples of the cloud into L and R (overwritten), at the sample rate sr
    void process(T* L, T* R, int n, double sr) {
        std::fill(L, L + n, (T)0);
        std::fill(R, R + n, (T)0);
        sr = std::max(1.0, sr);
        for (int i = 0; i < n; ++i) {
            if (p.on && --untilNext_ <= 0) {
                spawn(sr, i);
                double d = std::max(0.01, p.density), r = d * std::clamp(p.densityRnd, 0.0, 0.99);
                double lo = 1.0 / (d + r), hi = 1.0 / std::max(d - r, 1e-3);
                untilNext_ = std::max(1.0, (lo + rnd() * (hi - lo)) * sr);
            }
        }
        for (auto& pr : proto_)                  // the read position through a sound, n samples on
            if (pr.levels.empty() && pr.frames > 0) pr.readPos = wrap(pr.readPos + n * p.speed * pr.sr / sr, (double)pr.frames);
        for (auto& g : grains_) {
            if (!g.active) continue;
            // a prototype with no weight in the grain is not read at all
            bool use0 = g.morph < 1.0, use1 = g.morph > 0.0;
            const Proto& p0 = proto_[0];
            const Proto& p1 = proto_[1];
            const double len0 = p0.levels.empty() ? (double)std::max(1L, p0.frames) : (double)kTable;
            const double len1 = p1.levels.empty() ? (double)std::max(1L, p1.frames) : (double)kTable;
            // a prototype changed while the grain sounds (a sound for a waveform, square for sine, a shorter
            // sound): its table level and its phase are brought into the new prototype's range
            for (int c = 0; c < 2; ++c)
                if (!proto_[c].levels.empty()) g.level[c] = std::min(g.level[c], (int)proto_[c].levels.size() - 1);
            if (use0) g.phase[0] = wrap(g.phase[0], len0);
            if (use1) g.phase[1] = wrap(g.phase[1], len1);
            const double* t0 = p0.levels.empty() ? nullptr : p0.levels[(size_t)g.level[0]].data();
            const double* t1 = p1.levels.empty() ? nullptr : p1.levels[(size_t)g.level[1]].data();
            for (int i = g.delay; i < n; ++i) {
                double e = g.envPos;
                if (e >= kWin) { g.active = false; break; }
                double w = window_.at(e);
                double s = 0;
                if (use0) { s += (1.0 - g.morph) * (t0 ? readTable(t0, g.phase[0]) : readSound(p0, g.phase[0])); g.phase[0] = wrap(g.phase[0] + g.inc[0] * g.glide, len0); }
                if (use1) { s += g.morph * (t1 ? readTable(t1, g.phase[1]) : readSound(p1, g.phase[1])); g.phase[1] = wrap(g.phase[1] + g.inc[1] * g.glide, len1); }
                s *= w * g.amp;
                L[i] += (T)(s * g.left);
                R[i] += (T)(s * g.right);
                g.glide += g.glideStep;
                g.envPos += g.envStep;
            }
            g.delay = 0;
        }
    }

private:
    static constexpr double kPi_ = 3.14159265358979323846;
    struct Proto {
        std::vector<std::vector<double>> levels;   // a waveform's tables, richest first; empty for a sound
        std::vector<int> maxHarm;                  // the highest harmonic of each (0: not band-limited)
        const T* data = nullptr;
        long frames = 0;
        int stride = 1;
        double sr = 44100, readPos = 0;
    };
    struct Grain {
        bool active = false;
        int delay = 0, level[2] = {0, 0};
        double phase[2] = {0, 0}, inc[2] = {0, 0}, envPos = 0, envStep = 0, glide = 1, glideStep = 0;
        double amp = 0, left = 0, right = 0, morph = 0;
    };
    double rnd() {                                   // 0..1, Park-Miller as in the original
        rng_ = (unsigned)((unsigned long long)rng_ * 16807ULL % 2147483647ULL);
        return rng_ / 2147483647.0;
    }
    static double wrap(double x, double len) { return grains::wrapPos(x, len); }
    static double readTable(const double* t, double x) { return grains::readTable(t, x); }
    double readSound(const Proto& pr, double x) const { return grains::readWrapped(pr.data, pr.frames, pr.stride, x); }
    void spawn(double sr, int at) {
        int poly = std::clamp(p.poly, 1, kMaxPoly);
        int k = grains_.firstFree(poly);
        if (k < 0) return;
        Grain& g = grains_[k];
        double f = p.freq, fr = f * std::clamp(p.freqRnd, 0.0, 1.0);
        f = f - fr + rnd() * 2 * fr;
        double lenMs = std::max(1.0, p.length), lr = lenMs * std::clamp(p.lengthRnd, 0.0, 1.0);
        lenMs = lenMs - lr + rnd() * 2 * lr;
        if (p.grainlets > 0 && std::fabs(f) > 1e-3) lenMs = 1000.0 * p.grainlets / std::fabs(f);   // n periods
        double len = std::max(4.0, lenMs * sr / 1000.0);
        double gl = std::clamp(p.gliss, 0.0, 1.0);
        for (int c = 0; c < 2; ++c) {
            const Proto& pr = proto_[c];
            g.level[c] = 0;
            if (!pr.levels.empty()) {                        // a waveform: f in Hz
                g.inc[c] = f * kTable / sr;
                g.phase[c] = 0;
                // the richest table whose top harmonic stays under Nyquist at the highest the grain reaches
                double top = std::fabs(f) * (1.0 + gl);
                while (g.level[c] + 1 < (int)pr.levels.size() && pr.maxHarm[(size_t)g.level[c]] * top >= 0.5 * sr) ++g.level[c];
            } else {                                         // a sound: f is a speed ratio (or Hz, with its pitch)
                double pitch = p.pitch[c];
                g.inc[c] = (pitch > 0 ? f / pitch : f) * pr.sr / sr;
                g.phase[c] = pr.frames > 0 ? wrap(pr.readPos + at * p.speed * pr.sr / sr, (double)pr.frames) : 0.0;   // where the read position is at this sample
            }
        }
        double target = 1.0 + (rnd() * 2 - 1) * gl;          // glide to f * target
        g.glide = 1.0;
        g.glideStep = (target - 1.0) / len;
        g.envPos = 0;
        g.envStep = kWin / len;
        double j = std::clamp(p.jitter, 0.0, 1.0), jr = j * std::clamp(p.jitterRnd, 0.0, 1.0);
        double pan = std::clamp(j - jr + rnd() * 2 * jr, 0.0, 1.0);
        g.left = std::sqrt(pan);
        g.right = std::sqrt(1.0 - pan);
        g.amp = std::max(0.0, p.amp);
        g.morph = std::clamp(p.morph, 0.0, 1.0);
        g.delay = at;
        g.active = true;
    }

    Proto proto_[2];
    Proto next_[2];                              // tables built by prepareWave, or replaced by commitWave
    grains::GrainWindow window_;
    grains::GrainPool<Grain> grains_;
    double untilNext_ = 0;
    unsigned rng_ = 1;
};

}  // namespace dsp
}  // namespace musil
