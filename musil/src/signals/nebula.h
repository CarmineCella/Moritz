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
// seed gives the same cloud everywhere.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace musil {
namespace dsp {

template <class T>
class Nebula {
public:
    static constexpr int kTable = 4096, kWin = 4096, kMaxPoly = 1024;

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
        setWindow("hann");
        grains_.resize(kMaxPoly);
    }
    static bool isWave(const std::string& w) { return w == "sine" || w == "triangle" || w == "square" || w == "noise"; }
    // prototype k (0 or 1) is a waveform; false if the name is not one
    bool setWave(int k, const std::string& w) {
        if (!isWave(w)) return false;
        Proto& pr = proto_[k];
        pr.data = nullptr;
        pr.frames = 0;
        pr.table.assign((size_t)kTable + 1, 0);
        unsigned seed = 12345;
        for (int i = 0; i < kTable; ++i) {
            double t = (double)i / kTable, v = 0;
            if (w == "sine") v = std::sin(2 * kPi_ * t);
            else if (w == "noise") { seed = seed * 1103515245u + 12345u; v = (seed >> 8) / 8388608.0 - 1.0; }
            else
                for (int h = 1; h <= 63; h += 2)            // band-limited, odd harmonics
                    v += w == "square" ? std::sin(2 * kPi_ * h * t) / h
                                       : ((h / 2) % 2 ? -1.0 : 1.0) * std::sin(2 * kPi_ * h * t) / (h * h);
            pr.table[(size_t)i] = v;
        }
        double peak = 0;
        for (double v : pr.table) peak = std::max(peak, std::fabs(v));
        for (auto& v : pr.table) v /= std::max(peak, 1e-9);
        pr.table[(size_t)kTable] = pr.table[0];
        return true;
    }
    // prototype k is a sound: frames samples, every stride-th of data (one channel of interleaved
    // data), recorded at sr. The data must stay valid while process() runs; a real-time caller
    // sets it again before every block. frames 0: silence.
    void setSound(int k, const T* data, long frames, int stride, double sr) {
        Proto& pr = proto_[k];
        pr.table.clear();
        pr.data = data;
        pr.frames = data ? std::max(0L, frames) : 0;
        pr.stride = std::max(1, stride);
        pr.sr = sr;
        if (pr.frames > 0) pr.readPos = std::fmod(pr.readPos, (double)pr.frames);
    }
    bool isSound(int k) const { return proto_[k].table.empty(); }
    // the grain envelope: hann, hamming or blackman (an unknown name is hann)
    void setWindow(const std::string& type) {
        double a0 = .5, a1 = .5, a2 = 0;
        if (type == "hamming" || type == "hamm") { a0 = .54; a1 = .46; }
        else if (type == "blackman" || type == "black") { a0 = .42; a1 = .5; a2 = .08; }
        window_.assign((size_t)kWin + 1, 0);
        for (int i = 0; i <= kWin; ++i) {
            double t = 2 * kPi_ * i / kWin;
            window_[(size_t)i] = a0 - a1 * std::cos(t) + a2 * std::cos(2 * t);
        }
    }
    void seed(unsigned s) { rng_ = s ? s : 1; }
    // grains still sounding
    int active() const { int n = 0; for (auto& g : grains_) n += g.active; return n; }

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
            for (auto& pr : proto_)              // the read position through a sound
                if (pr.table.empty() && pr.frames > 0) {
                    pr.readPos += p.speed * pr.sr / sr;
                    double len = (double)pr.frames;
                    pr.readPos = std::fmod(pr.readPos, len);
                    if (pr.readPos < 0) pr.readPos += len;
                }
        }
        for (auto& g : grains_) {
            if (!g.active) continue;
            for (int i = g.delay; i < n; ++i) {
                double e = g.envPos;
                if (e >= kWin) { g.active = false; break; }
                int ei = (int)e;
                double w = window_[(size_t)ei] + (e - ei) * (window_[(size_t)ei + 1] - window_[(size_t)ei]);
                double s = (1.0 - g.morph) * read(proto_[0], g.phase[0]) + g.morph * read(proto_[1], g.phase[1]);
                s *= w * g.amp;
                L[i] += (T)(s * g.left);
                R[i] += (T)(s * g.right);
                for (int k = 0; k < 2; ++k) g.phase[k] += g.inc[k] * g.glide;
                g.glide += g.glideStep;
                g.envPos += g.envStep;
            }
            g.delay = 0;
        }
    }

private:
    static constexpr double kPi_ = 3.14159265358979323846;
    struct Proto {
        std::vector<double> table;          // a waveform, or empty for a sound
        const T* data = nullptr;
        long frames = 0;
        int stride = 1;
        double sr = 44100, readPos = 0;
    };
    struct Grain {
        bool active = false;
        int delay = 0;
        double phase[2] = {0, 0}, inc[2] = {0, 0}, envPos = 0, envStep = 0, glide = 1, glideStep = 0;
        double amp = 0, left = 0, right = 0, morph = 0;
    };
    double rnd() {                                   // 0..1, Park-Miller as in the original
        rng_ = (unsigned)((unsigned long long)rng_ * 16807ULL % 2147483647ULL);
        return rng_ / 2147483647.0;
    }
    double read(const Proto& pr, double ph) const {
        if (!pr.table.empty()) {
            double x = std::fmod(ph, (double)kTable);
            if (x < 0) x += kTable;
            int i = (int)x;
            return pr.table[(size_t)i] + (x - i) * (pr.table[(size_t)i + 1] - pr.table[(size_t)i]);
        }
        if (pr.frames <= 0) return 0;
        double x = std::fmod(ph, (double)pr.frames);
        if (x < 0) x += pr.frames;
        long i = (long)x;
        double a = (double)pr.data[(size_t)i * pr.stride], c = (double)pr.data[(size_t)((i + 1) % pr.frames) * pr.stride];
        return a + (x - i) * (c - a);
    }
    void spawn(double sr, int at) {
        int poly = std::clamp(p.poly, 1, kMaxPoly);
        for (int k = 0; k < poly; ++k) {
            Grain& g = grains_[(size_t)k];
            if (g.active) continue;
            double f = p.freq, fr = f * std::clamp(p.freqRnd, 0.0, 1.0);
            f = f - fr + rnd() * 2 * fr;
            double lenMs = std::max(1.0, p.length), lr = lenMs * std::clamp(p.lengthRnd, 0.0, 1.0);
            lenMs = lenMs - lr + rnd() * 2 * lr;
            if (p.grainlets > 0 && std::fabs(f) > 1e-3) lenMs = 1000.0 * p.grainlets / std::fabs(f);   // n periods
            double len = std::max(4.0, lenMs * sr / 1000.0);
            for (int c = 0; c < 2; ++c) {
                const Proto& pr = proto_[c];
                if (!pr.table.empty()) {                         // a waveform: f in Hz
                    g.inc[c] = f * kTable / sr;
                    g.phase[c] = 0;
                } else {                                         // a sound: f is a speed ratio (or Hz, with its pitch)
                    double pitch = p.pitch[c];
                    g.inc[c] = (pitch > 0 ? f / pitch : f) * pr.sr / sr;
                    g.phase[c] = pr.readPos;
                }
            }
            double gl = std::clamp(p.gliss, 0.0, 1.0);
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
            return;
        }
    }

    Proto proto_[2];
    std::vector<double> window_;
    std::vector<Grain> grains_;
    double untilNext_ = 0;
    unsigned rng_ = 1;
};

}  // namespace dsp
}  // namespace musil
