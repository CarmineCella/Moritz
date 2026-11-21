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
//
// StreamingYin (at the end) runs it on a stream, a frame every few milliseconds: the detector of the
// harmonizer (psola.h).
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "biquad.h"
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

// A biquad section run in direct form II transposed, with normalised coefficients (a0 = 1): the low-pass of the
// streaming YIN and of the harmonizer's pitch marks
struct BiquadSection {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set(const BiquadCoeffs& c) {
        BiquadCoeffs n = c.normalized();
        b0 = n.b0; b1 = n.b1; b2 = n.b2; a1 = n.a1; a2 = n.a2;
    }
    void reset() { z1 = z2 = 0; }
    double tick(double x) {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// YIN on a stream (the harmonizer's detector, psola.h): the samples go into a ring; every hop (5 ms) the last frame
// is estimated by Yin, so the estimate is of the newest samples. The frame is as short as YIN allows for fmin (a
// window and as many lags as the longest period, 2 / fmin seconds); with `local` (step 6, the best local
// estimate) it is half a longest period longer, so windows can shift. Options:
//   decimate   1, 2 or 4: the signal is low-passed and decimated first (a quarter of the work at 4 for a low voice);
//   lowpass    without decimation, a low-pass at 4 fmax (removes what no f0 in the range needs);
//   gate       a frame whose RMS is under it (linear) is unvoiced;
//   median     the f0 given is the median of the last three voiced estimates (one wrong frame is not heard);
// Allocates in init only; push() and the estimates allocate nothing (an audio thread can run it).
class StreamingYin {
public:
    struct Config {
        double sr = 44100, fmin = 70, fmax = 1000, hopMs = 5, threshold = 0.15, gate = 0.003;
        int decimate = 1;
        bool lowpass = true, local = false, median = true;
    };
    void init(const Config& c) {
        cfg_ = c;
        dec_ = c.decimate >= 4 ? 4 : c.decimate >= 2 ? 2 : 1;
        cfg_.decimate = dec_;
        double sr = c.sr / dec_;
        cfg_.fmax = std::min(c.fmax, 0.45 * sr);
        int tauMax = (int)std::ceil(sr / std::max(1.0, c.fmin));
        int len = 2 * (tauMax + 2) + (c.local ? tauMax : 0);
        yin_.init(len, sr, c.fmin, cfg_.fmax);
        len_ = len;
        frame_.assign((size_t)len, 0.0);
        size_t ring = 1;
        while (ring < (size_t)len + 16) ring <<= 1;
        ring_.assign(ring, 0.0);
        mask_ = ring - 1;
        hop_ = std::max(1, (int)std::lround(c.hopMs * 1e-3 * sr));
        lp_[0].reset(); lp_[1].reset();
        filtered_ = dec_ > 1 || c.lowpass;
        double cut = dec_ > 1 ? 0.4 * sr : std::min(4 * cfg_.fmax, 0.45 * c.sr);
        lp_[0].set(rbj(BiquadType::Lowpass, c.sr, cut, 0.5412));     // fourth-order Butterworth
        lp_[1].set(rbj(BiquadType::Lowpass, c.sr, cut, 1.3066));
        reset();
    }
    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.0);
        lp_[0].reset(); lp_[1].reset();
        w_ = 0; phase_ = 0; untilHop_ = hop_;
        f0_ = periodicity_ = 0; voiced_ = false; level_ = 0;
        hist_[0] = hist_[1] = hist_[2] = 0; nhist_ = 0;
    }
    void setThreshold(double t) { cfg_.threshold = std::clamp(t, 0.001, 1.0); }
    void setGate(double g) { cfg_.gate = std::max(0.0, g); }
    bool ready() const { return yin_.ready(); }
    const Config& config() const { return cfg_; }
    // the input samples between the newest and the centre of the frame estimated
    int lag() const { return len_ / 2 * dec_; }
    int hop() const { return hop_ * dec_; }
    // one input sample; true when it completed a hop (a new estimate)
    template <class T>
    bool push(T in) {
        double x = (double)in;
        if (!std::isfinite(x)) x = 0;
        if (filtered_) x = lp_[1].tick(lp_[0].tick(x));
        if (++phase_ < dec_) return false;
        phase_ = 0;
        ring_[w_ & mask_] = x;
        ++w_;
        if (--untilHop_ > 0) return false;
        untilHop_ = hop_;
        estimate();
        return true;
    }
    double f0() const { return f0_; }                    // Hz (median of three when asked), 0 unvoiced
    double rawF0() const { return raw_; }                // the last frame's own estimate
    double periodicity() const { return periodicity_; }
    double level() const { return level_; }              // the last frame's RMS
    bool voiced() const { return voiced_; }

private:
    void estimate() {
        size_t start = w_ - (size_t)len_;
        double e = 0;
        for (int k = 0; k < len_; ++k) {
            double v = ring_[(start + (size_t)k) & mask_];
            frame_[(size_t)k] = v;
            e += v * v;
        }
        level_ = std::sqrt(e / std::max(1, len_));
        PitchEstimate p;
        if (level_ >= cfg_.gate && yin_.ready()) p = yin_.estimate(frame_.data(), cfg_.threshold);
        raw_ = p.f0;
        periodicity_ = p.periodicity;
        if (!(p.f0 > 0)) {
            voiced_ = false;
            f0_ = 0;
            nhist_ = 0;
            return;
        }
        voiced_ = true;
        hist_[2] = hist_[1]; hist_[1] = hist_[0]; hist_[0] = p.f0;
        nhist_ = std::min(3, nhist_ + 1);
        if (!cfg_.median || nhist_ < 3) { f0_ = p.f0; return; }
        double a = hist_[0], b = hist_[1], c = hist_[2];
        f0_ = std::max(std::min(a, b), std::min(std::max(a, b), c));
    }
    Config cfg_;
    Yin yin_;
    BiquadSection lp_[2];
    std::vector<double> ring_, frame_;
    size_t mask_ = 0, w_ = 0;
    int len_ = 0, hop_ = 1, dec_ = 1, phase_ = 0, untilHop_ = 1, nhist_ = 0;
    bool filtered_ = false, voiced_ = false;
    double f0_ = 0, raw_ = 0, periodicity_ = 0, level_ = 0, hist_[3] = {0, 0, 0};
};

}  // namespace dsp
}  // namespace musil
