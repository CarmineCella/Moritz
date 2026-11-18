// filters.h — the state-variable filter of Musil's svf (and its live node) and of Moritz's svf~.
// Self-contained (the standard library only).
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <algorithm>
#include <cmath>

#include "denormal.h"

namespace musil {
namespace dsp {

// State-variable filter, topology-preserving (Simper): low, high, band and notch outputs at once.
// q in 0..1 is the resonance (as in MSP's svf~): 0 a gentle slope, towards 1 a sharp peak. The
// frequency is kept within 1 Hz and 0.49 sr. Stable under fast modulation of both.
// Gains (k = 2 - 1.98 q, the damping): low and high pass 1 in their passband and 1/k at the cutoff; the
// band output is the unnormalised band-pass, whose gain at the centre is 1/k too: 0.5 (-6 dB) at q 0, 1
// (0 dB) at q 0.5, 50 (+34 dB) at q 1. That is svf~'s established behaviour, kept: scale the band output
// by k (or lower q) for a band-pass of unit peak gain. The notch is low + high (unit gain away from it).
class Svf {
public:
    void set(double f, double q, double sr) {
        f = std::clamp(f, 1.0, sr * 0.49);
        k_ = std::max(2.0 - 2.0 * std::clamp(q, 0.0, 1.0) * 0.99, 0.02);
        double g = std::tan(3.14159265358979323846 * f / sr);
        a1_ = 1.0 / (1.0 + g * (g + k_));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }
    void reset() { ic1_ = ic2_ = 0.0; }
    // one sample: the four outputs
    void tick(double v0, double& lo, double& hi, double& band, double& notch) {
        double v3 = v0 - ic2_;
        double v1 = a1_ * ic1_ + a2_ * v3;
        double v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0 * v1 - ic1_;
        ic2_ = 2.0 * v2 - ic2_;
        lo = v2;
        band = v1;
        hi = v0 - k_ * v1 - v2;
        notch = lo + hi;
    }
    // n samples with the current settings; any output may be null
    template <class T>
    void process(const T* in, T* lo, T* hi, T* band, T* notch, int n) {
        DenormalGuard ftz;                         // a decaying resonance: no denormals (x86)
        for (int i = 0; i < n; ++i) {
            double l, h, b, no;
            tick((double)in[i], l, h, b, no);
            if (lo) lo[i] = (T)l;
            if (hi) hi[i] = (T)h;
            if (band) band[i] = (T)b;
            if (notch) notch[i] = (T)no;
        }
        if (!std::isfinite(ic1_) || !std::isfinite(ic2_)) reset();
    }

private:
    double k_ = 2, a1_ = 1, a2_ = 0, a3_ = 0, ic1_ = 0, ic2_ = 0;
};

}  // namespace dsp
}  // namespace musil
