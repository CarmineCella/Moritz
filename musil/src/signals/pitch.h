// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// pitch.h — the fundamental frequency of a frame by YIN (de Cheveigné & Kawahara 2002), the one
// implementation of Musil's yin, yin-track and descriptors and of Moritz's descriptors~. The samples
// may be float or double; the arithmetic is in double. It needs nothing but the standard library.
//
// A frame of len samples is read in windows of W = len / 2 samples:
//   1-2. the difference function d(t) = sum over the window of (x[j] - x[j + t])^2, for the lags t up to
//        the longest period asked, computed by FFT (the window's energy plus the energy of the window t
//        later, minus twice their cross-correlation): O(len log len) instead of O(len^2);
//   3.   the cumulative mean normalised difference d'(t) = d(t) / ((1/t) sum_{j=1..t} d(j)), d'(0) = 1:
//        near 0 at the period of a periodic sound, about 1 for noise;
//   4.   the absolute threshold: the first lag whose d' dips under the threshold (0.1), followed down to
//        its local minimum; no dip under it: unvoiced (f0 0);
//   5.   parabolic interpolation around that lag: the value of d' at its minimum (for the choices), the period
//        between samples from d's (d' would bias it, as the paper notes);
//   6.   the best local estimate: the same is done for windows shifted within the room the frame leaves
//        (up to half the longest period either side of the centre); when one of them finds a deeper dip,
//        the centre window is searched again within 20% of that period.
// The periodicity is 1 - d' at the chosen lag (the aperiodicity's complement), 0..1; a silent frame
// has f0 0 and periodicity 0. The longest period searched is the smaller of sr / fmin and len / 2 - 2
// samples, the shortest sr / fmax (2 at least).
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "fft.h"

namespace musil {
namespace dsp {

struct PitchEstimate {
    double f0 = 0;            // Hz; 0 when unvoiced
    double periodicity = 0;   // 1 - aperiodicity, 0..1
};

class Yin {
public:
    // a frame of len samples at sr, periods between sr / fmax and sr / fmin; allocates (not on an audio thread)
    void init(int len, double sr, double fmin = 50.0, double fmax = 4000.0) {
        len_ = std::max(0, len);
        sr_ = sr;
        fmin_ = fmin;
        fmax_ = fmax;
        w_ = len_ / 2;
        tauMax_ = 0;
        tauMin_ = 2;
        lags_ = room_ = 0;
        if (!(sr > 0) || !std::isfinite(sr)) return;                  // not ready
        int fit = len_ - w_ - 2;                                       // lags 0 .. tauMax + 1 must fit after the window
        double longest = fmin > 0 ? std::ceil(sr / fmin) : (double)fit;
        tauMax_ = (int)std::min<double>(fit, std::max(0.0, longest));
        double shortest = fmax > 0 ? std::floor(sr / fmax) : 2.0;
        tauMin_ = (int)std::clamp(shortest, 2.0, 1e9);
        lags_ = std::max(0, tauMax_ + 2);
        room_ = std::max(0, len_ - w_ - lags_);
        if (!ready()) return;
        m_ = (int)next_pow2((size_t)(w_ + lags_));
        fft_.init(m_);
        a_.assign((size_t)m_, 0.0);
        b_.assign((size_t)m_, 0.0);
        ar_.assign((size_t)m_ / 2 + 1, 0.0);
        ai_.assign((size_t)m_ / 2 + 1, 0.0);
        br_.assign((size_t)m_ / 2 + 1, 0.0);
        bi_.assign((size_t)m_ / 2 + 1, 0.0);
        c_.assign((size_t)m_, 0.0);
        e_.assign((size_t)(w_ + lags_) + 1, 0.0);
        centre_.assign((size_t)lags_, 1.0);
        other_.assign((size_t)lags_, 1.0);
        rawCentre_.assign((size_t)lags_, 0.0);
        rawOther_.assign((size_t)lags_, 0.0);
    }
    bool matches(int len, double sr, double fmin, double fmax) const {
        return len == len_ && sr == sr_ && fmin == fmin_ && fmax == fmax_;
    }
    // the frame leaves room for at least two lags in the range
    bool ready() const { return w_ >= 2 && tauMax_ >= tauMin_ + 1 && sr_ > 0 && std::isfinite(sr_); }
    int length() const { return len_; }
    double lowest() const { return tauMax_ > 0 ? sr_ / tauMax_ : 0.0; }    // the lowest f0 it can find
    double highest() const { return sr_ / tauMin_; }
    // the cumulative mean normalised difference of the centre window of the last estimate (lags 0 .. tauMax + 1)
    const std::vector<double>& cmndf() const { return centre_; }

    // f0 and periodicity of the frame x (len samples); allocates nothing
    template <class T>
    PitchEstimate estimate(const T* x, double threshold = 0.1) {
        PitchEstimate out;
        if (!ready()) return out;
        const int centre = room_ / 2;
        if (!difference(x + centre, centre_, rawCentre_)) return out;  // silence
        Pick c = pick(centre_, tauMin_, tauMax_, threshold);
        Pick best = c;
        int bestAt = centre;
        double bestLag = 0, bestValue = 0;
        if (c.voiced) refine(centre_, rawCentre_, c.tau, bestLag, bestValue);
        // step 6: the windows around the centre, up to half the longest period away, within the frame
        int lo = std::max(0, centre - tauMax_ / 2), hi = std::min(room_, centre + tauMax_ / 2);
        if (hi > lo) {
            for (int k = 0; k <= 4; ++k) {
                int at = lo + (int)((long)(hi - lo) * k / 4);
                if (at == centre || !difference(x + at, other_, rawOther_)) continue;
                Pick p = pick(other_, tauMin_, tauMax_, threshold);
                if (p.voiced && (!best.voiced || other_[(size_t)p.tau] < best.value)) {
                    best = p;
                    bestAt = at;
                    refine(other_, rawOther_, p.tau, bestLag, bestValue);
                }
            }
        }
        if (!best.voiced) {                                            // no window dips under the threshold
            out.periodicity = std::clamp(1.0 - c.value, 0.0, 1.0);
            return out;
        }
        double lag = bestLag, value = bestValue;
        if (bestAt != centre) {
            // the centre again, within 20% of the best period; failing that the centre's own estimate,
            // and only then the neighbour's
            int rlo = std::max(tauMin_, (int)std::floor(.8 * best.tau)), rhi = std::min(tauMax_, (int)std::ceil(1.2 * best.tau));
            Pick r = rhi >= rlo ? pick(centre_, rlo, rhi, threshold) : Pick{};
            if (r.voiced) refine(centre_, rawCentre_, r.tau, lag, value);
            else if (c.voiced) refine(centre_, rawCentre_, c.tau, lag, value);
        }
        if (!(lag > 0)) return out;
        out.f0 = sr_ / lag;
        out.periodicity = std::clamp(1.0 - value, 0.0, 1.0);
        return out;
    }

private:
    struct Pick {
        int tau = 0;
        double value = 1.0;
        bool voiced = false;
    };
    // steps 1-3 for the window starting at x: d into raw, d' into dp (lags_ values); false when the frame is silent
    template <class T>
    bool difference(const T* x, std::vector<double>& dp, std::vector<double>& raw) {
        const int span = w_ + lags_;
        e_[0] = 0;
        for (int k = 0; k < span; ++k) {
            double v = (double)x[k];
            e_[(size_t)k + 1] = e_[(size_t)k] + v * v;
        }
        if (!(e_[(size_t)span] > 1e-20 * span) || !std::isfinite(e_[(size_t)span])) {   // under -200 dB, or broken
            std::fill(dp.begin(), dp.end(), 1.0);
            std::fill(raw.begin(), raw.end(), 0.0);
            return false;
        }
        for (int k = 0; k < m_; ++k) {
            a_[(size_t)k] = k < w_ ? (double)x[k] : 0.0;
            b_[(size_t)k] = k < span ? (double)x[k] : 0.0;
        }
        fft_.forward(a_.data(), ar_.data(), ai_.data());
        fft_.forward(b_.data(), br_.data(), bi_.data());
        for (int k = 0; k <= m_ / 2; ++k) {                           // conj(A) B: the cross-correlation
            double re = ar_[(size_t)k] * br_[(size_t)k] + ai_[(size_t)k] * bi_[(size_t)k];
            double im = ar_[(size_t)k] * bi_[(size_t)k] - ai_[(size_t)k] * br_[(size_t)k];
            ar_[(size_t)k] = re;
            ai_[(size_t)k] = im;
        }
        fft_.inverse(ar_.data(), ai_.data(), c_.data());
        const double r0 = e_[(size_t)w_], scale = 1.0 / m_;
        dp[0] = 1.0;
        raw[0] = 0.0;
        double run = 0;
        for (int t = 1; t < lags_; ++t) {
            double d = r0 + (e_[(size_t)(t + w_)] - e_[(size_t)t]) - 2.0 * c_[(size_t)t] * scale;
            d = std::max(0.0, d);
            raw[(size_t)t] = d;
            run += d;
            dp[(size_t)t] = run > 0 ? d * t / run : 1.0;
        }
        return true;
    }
    // step 4 within lags lo..hi
    static Pick pick(const std::vector<double>& dp, int lo, int hi, double threshold) {
        Pick p;
        for (int t = lo; t <= hi; ++t)
            if (dp[(size_t)t] < threshold) {
                while (t + 1 <= hi && dp[(size_t)t + 1] < dp[(size_t)t]) ++t;
                p.tau = t;
                p.value = dp[(size_t)t];
                p.voiced = true;
                return p;
            }
        p.tau = lo;
        for (int t = lo; t <= hi; ++t)
            if (dp[(size_t)t] < dp[(size_t)p.tau]) p.tau = t;
        p.value = dp[(size_t)p.tau];
        return p;
    }
    // step 5: parabolas through tau - 1, tau, tau + 1: the lag between samples (d's) and the value of d' there
    void refine(const std::vector<double>& dp, const std::vector<double>& raw, int tau, double& lag, double& value) const {
        lag = tau;
        value = dp[(size_t)tau];
        if (tau < 1 || tau + 1 >= lags_) return;
        double a = raw[(size_t)tau - 1], b = raw[(size_t)tau], c = raw[(size_t)tau + 1], den = a - 2 * b + c;
        if (den > 0) lag = tau + std::clamp(.5 * (a - c) / den, -.5, .5);
        a = dp[(size_t)tau - 1]; b = dp[(size_t)tau]; c = dp[(size_t)tau + 1]; den = a - 2 * b + c;
        if (den > 0) {
            double off = std::clamp(.5 * (a - c) / den, -.5, .5);
            value = std::clamp(b - .25 * (a - c) * off, 0.0, b);
        }
    }

    int len_ = 0, w_ = 0, tauMin_ = 2, tauMax_ = 0, lags_ = 0, room_ = 0, m_ = 0;
    double sr_ = 0, fmin_ = 0, fmax_ = 0;
    RealFFT<double> fft_;
    std::vector<double> a_, b_, ar_, ai_, br_, bi_, c_, e_, centre_, other_, rawCentre_, rawOther_;
};

}  // namespace dsp
}  // namespace musil
