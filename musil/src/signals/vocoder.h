// vocoder.h — the phase vocoder engine of sparkle, one frame at a time: Musil's pvoc runs it over
// a whole sound, Moritz's sparkle~ in real time. Self-contained (the standard library and fft.h).
//
// Copyright (c) 2011-2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// A frame is the window of samples at the read position, `now`, and the window D samples before
// it, `prev`: the frequency of every bin is measured from the phase difference between the two, so
// the caller may move the read position as it likes (stretch, freeze, play backwards, jump) and the
// analysis stays right. The synthesis phases accumulate from frame to frame by the synthesis hop I,
// and are locked around the spectral peaks (Laroche-Dolson): every bin keeps its analysed offset
// from the peak of its region. Then, as in sparkle:
//   - pitch shift by P, each peak's region moved as a whole to the peak's new place (its level kept);
//   - formant (spectral envelope) preservation, or a move of the formants by F: the envelope is the
//     cepstral smoothing of the log spectrum's peak-to-peak interpolation (a one-pass stand-in for
//     the iterated "true envelope"), and each bin is scaled by the envelope where it came from;
//   - denoising: bins weaker than a threshold are dropped;
//   - two phase effects: robot (zero phases), whisper (random phases);
//   - three cross-syntheses with a second sound: 1 multiplicative (the geometric mean of the two
//     magnitudes, phases mixed by the amount), 2 spectral flattener (the second sound's envelope
//     imposed on the first), 3 morphing (magnitudes interpolated, phases switching at the half).
// The window is a periodic Hann of `window` samples, centred in an FFT of next_pow2(window) << pad
// points (zero-phase windowing). The output frame is windowed again and scaled so that frames
// overlap-added every I samples keep the level.
#pragma once

#include "fft.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

namespace musil {
namespace dsp {

class Vocoder {
public:
    Vocoder(int window, int pad) { init(window, pad); }

    // settings, read at every frame
    int envelope = 0;      // cepstral coefficients of the envelope (0: no formant preservation); about sr / (2 f0), as envelope.h says
    int phaseMode = 0;     // 1 robot, 2 whisper
    int crossMode = 0;     // 1 multiplicative, 2 spectral flattener, 3 morphing (0 off)

    int window() const { return Wn_; }
    int size() const { return N_; }
    // the magnitude of a full-scale sine in a frame: the unit of the threshold
    double fullScale() const { return wsum_ / 2; }
    void seed(uint32_t s) { rng_.seed(s); }
    void reset() { std::fill(oldsyn_.begin(), oldsyn_.end(), 0.0); fresh_ = true; }

    // now, prev: `window` samples at the read position and D samples before it. otherNow,
    // otherPrev: the same for the second sound (cross-synthesis; null when crossMode is 0).
    // I: synthesis hop (negative: the phases walk back from the last frame, for frames before a start; the
    // frame comes out negated); P: pitch ratio; F: formant ratio; threshold: bins below threshold times a
    // full-scale sine are dropped (0: none); A: cross amount, 0..1. out: `window` samples to
    // overlap-add at the synthesis position.
    template <class T>
    void process(const T* now, const T* prev, const T* otherNow, const T* otherPrev, double D, double I,
                 double P, double F, double threshold, double A, T* out) {
        const size_t N2 = (size_t)N2_;
        analyse(now, mags_, phase_);
        analyse(prev, pmags_, pphase_);
        if (threshold > 0) {
            double t = threshold * fullScale();
            for (size_t k = 0; k < N2; ++k) if (mags_[k] < t) mags_[k] = 0;
        }
        bool cross = crossMode >= 1 && crossMode <= 3 && otherNow && otherPrev;
        if (cross) {
            A = std::clamp(A, 0.0, 1.0);
            analyse(otherNow, omags_, ophase_);
            analyse(otherPrev, pmags_, opphase_);
            if (crossMode == 1) {
                for (size_t k = 0; k < N2; ++k) {
                    mags_[k] = std::sqrt(mags_[k] * omags_[k]);
                    phase_[k] = (1 - A) * phase_[k] + A * ophase_[k];
                    pphase_[k] = (1 - A) * pphase_[k] + A * opphase_[k];
                }
            } else if (crossMode == 2) {
                int order = envelope > 0 ? envelope : 40;
                logEnvelope(mags_, env1_, order);
                logEnvelope(omags_, env2_, order);
                for (size_t k = 0; k < N2; ++k) mags_[k] *= (1 - A) + A * std::exp(env2_[k] - env1_[k]);
            } else {
                for (size_t k = 0; k < N2; ++k) {
                    mags_[k] += (omags_[k] - mags_[k]) * A;
                    if (A >= 0.5) { phase_[k] = ophase_[k]; pphase_[k] = opphase_[k]; }
                }
            }
        }
        // the frequency of every bin, as a phase increment per sample (times the pitch ratio)
        for (size_t k = 0; k < N2; ++k) {
            double omega = kTwoPi * (double)k * D / N_;
            inc_[k] = P * (omega + princarg(phase_[k] - pphase_[k] - omega)) / D;
        }
        // phase locking: the peaks advance from their last synthesis phase, the bins of a peak's
        // region (up to half way to the next peak) keep their analysed offset from it
        peaks_.clear();
        for (size_t k = 1; k + 1 < N2; ++k) if (mags_[k] > mags_[k - 1] && mags_[k] > mags_[k + 1]) peaks_.push_back(k);
        if (peaks_.empty()) peaks_.push_back(0);
        size_t r = 0, next = peaks_.size() > 1 ? (peaks_[0] + peaks_[1]) / 2 + 1 : N2;
        for (size_t k = 0; k < N2; ++k) {
            while (k >= next && r + 1 < peaks_.size()) {
                ++r;
                next = r + 1 < peaks_.size() ? (peaks_[r] + peaks_[r + 1]) / 2 + 1 : N2;
            }
            owner_[k] = peaks_[r];
        }
        if (fresh_) psi_ = phase_;          // the first frame sounds with its own phases
        else
            for (size_t k = 0; k < N2; ++k) {
                size_t pk = owner_[k];
                psi_[k] = oldsyn_[pk] + I * inc_[pk] + (phase_[k] - phase_[pk]);
            }
        oldsyn_ = psi_;
        fresh_ = false;
        // the envelope: bin k will sound at k P, where the envelope should be the original one
        // at k P / F; scale it by the ratio of the two
        if (envelope > 0 && (P != 1 || F != 1)) {
            logEnvelope(mags_, env1_, envelope);
            double r2 = P / F;
            for (size_t k = 0; k < N2; ++k) {
                double src = std::min((double)k * r2, (double)(N2 - 1));
                size_t i = (size_t)src;
                double e = env1_[i] + (src - (double)i) * (env1_[std::min(i + 1, N2 - 1)] - env1_[i]);
                mags_[k] *= std::exp(e - env1_[k]);
            }
        }
        // pitch shift: every peak's region moved as a whole (Laroche-Dolson), its peak from bin k to the bin
        // nearest k P; the shape of each partial's lobe is kept, so its level is (resampling the spectrum bin by
        // bin would widen or thin the lobes: +1.7 dB an octave up, -4.2 dB an octave down). The phases already
        // advance at P times the analysed frequencies; regions that land on the same bins add as complex values
        if (P != 1) {
            std::fill(newm_.begin(), newm_.end(), 0.0);
            std::fill(newp_.begin(), newp_.end(), 0.0);
            for (size_t k = 0; k < N2; ++k) {
                long pk = (long)owner_[k];
                long j = (long)k + std::lround((double)pk * P) - pk;
                if (j < 0 || j >= (long)N2 || mags_[k] == 0) continue;
                newm_[(size_t)j] += mags_[k] * std::cos(psi_[k]);
                newp_[(size_t)j] += mags_[k] * std::sin(psi_[k]);
            }
            for (size_t k = 0; k < N2; ++k) {
                double a = newm_[k], b = newp_[k];
                mags_[k] = std::sqrt(a * a + b * b);
                psi_[k] = mags_[k] > 0 ? std::atan2(b, a) : 0.0;
            }
        }
        if (phaseMode == 1) std::fill(psi_.begin(), psi_.end(), 0.0);
        else if (phaseMode == 2) for (auto& p : psi_) p = kTwoPi * (rng_() / 4294967296.0);
        // synthesis: a real spectrum, back to time, unshifted, windowed
        std::fill(re_.begin(), re_.end(), 0.0);
        std::fill(im_.begin(), im_.end(), 0.0);
        for (size_t k = 0; k < N2; ++k) {
            double a = mags_[k] * std::cos(psi_[k]), b = mags_[k] * std::sin(psi_[k]);
            re_[k] = a; im_[k] = b;
            if (k) { re_[N_ - k] = a; im_[N_ - k] = -b; }
        }
        fft_.inverse(re_.data(), im_.data());
        double scale = I / (sum2_ * N_);
        for (int k = 0; k < Wn_; ++k) out[k] = (T)(re_[(size_t)((offset_ + k + N2_) % N_)] * w_[(size_t)k] * scale);
    }

private:
    void init(int window, int pad) {
        Wn_ = std::max(8, window);
        N_ = (int)(next_pow2((size_t)Wn_) << std::max(0, pad));
        N2_ = N_ / 2;
        offset_ = (N_ - Wn_) / 2;
        w_.resize((size_t)Wn_);
        wsum_ = sum2_ = 0;
        for (int k = 0; k < Wn_; ++k) {
            w_[(size_t)k] = 0.5 - 0.5 * std::cos(kTwoPi * k / Wn_);
            wsum_ += w_[(size_t)k];
            sum2_ += w_[(size_t)k] * w_[(size_t)k];
        }
        fft_.init(N_);
        for (auto* v : {&re_, &im_, &tmp_, &tmpi_}) v->assign((size_t)N_, 0.0);
        for (auto* v : {&mags_, &phase_, &pmags_, &pphase_, &omags_, &ophase_, &opphase_, &psi_, &oldsyn_, &inc_,
                        &newm_, &newp_, &env1_, &env2_})
            v->assign((size_t)N2_, 0.0);
        owner_.assign((size_t)N2_, 0);
        peaks_.reserve((size_t)N2_);
    }
    static double princarg(double p) { return p - kTwoPi * std::round(p / kTwoPi); }
    // the windowed frame, centred in the FFT and shifted so that its centre is at time 0
    template <class T>
    void analyse(const T* x, std::vector<double>& m, std::vector<double>& ph) {
        std::fill(re_.begin(), re_.end(), 0.0);
        std::fill(im_.begin(), im_.end(), 0.0);
        for (int k = 0; k < Wn_; ++k) re_[(size_t)((offset_ + k + N2_) % N_)] = (double)x[k] * w_[(size_t)k];
        fft_.forward(re_.data(), im_.data());
        for (size_t k = 0; k < (size_t)N2_; ++k) {
            m[k] = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]);
            ph[k] = std::atan2(im_[k], re_[k]);
        }
    }
    // the smooth log envelope of a half spectrum: the log magnitudes replaced by their peak-to-peak
    // interpolation (so the envelope rides the harmonics instead of averaging peaks and valleys),
    // then liftered, keeping `order` cepstral coefficients
    void logEnvelope(const std::vector<double>& half, std::vector<double>& env, int order) {
        const size_t N2 = (size_t)N2_, N = (size_t)N_;
        double mx = 1e-12;
        for (double v : half) mx = std::max(mx, v);
        double fl = 1e-4 * mx;
        env.resize(N2);
        for (size_t k = 0; k < N2; ++k) env[k] = std::log(std::max(half[k], fl));
        size_t last = 0;
        bool have = false;
        for (size_t k = 1; k + 1 < N2; ++k)
            if (env[k] > env[k - 1] && env[k] > env[k + 1]) {
                if (!have) { for (size_t j = 0; j < k; ++j) env[j] = env[k]; have = true; }
                else for (size_t j = last + 1; j < k; ++j) env[j] = env[last] + (env[k] - env[last]) * (double)(j - last) / (double)(k - last);
                last = k;
            }
        if (have) for (size_t j = last + 1; j < N2; ++j) env[j] = env[last];
        std::fill(tmp_.begin(), tmp_.end(), 0.0);
        std::fill(tmpi_.begin(), tmpi_.end(), 0.0);
        for (size_t k = 0; k < N2; ++k) { tmp_[k] = env[k]; if (k) tmp_[N - k] = env[k]; }
        tmp_[N2] = env[N2 - 1];
        fft_.inverse(tmp_.data(), tmpi_.data());
        size_t o = (size_t)std::max(0, order);
        for (size_t k = 0; k < N; ++k) {
            bool keep = k <= o || k >= N - o;
            tmp_[k] = keep ? tmp_[k] / (double)N : 0;
            tmpi_[k] = 0;
        }
        fft_.forward(tmp_.data(), tmpi_.data());
        for (size_t k = 0; k < N2; ++k) env[k] = tmp_[k];
    }

    int Wn_ = 0, N_ = 0, N2_ = 0, offset_ = 0;
    double wsum_ = 0, sum2_ = 0;
    FFT<double> fft_;
    std::vector<double> w_, re_, im_, tmp_, tmpi_, mags_, phase_, pmags_, pphase_, omags_, ophase_, opphase_, psi_, oldsyn_, inc_,
        newm_, newp_, env1_, env2_;
    std::vector<size_t> peaks_, owner_;
    std::mt19937 rng_{1};
    bool fresh_ = true;
};

// ---- over a whole sound: Musil's pvoc, and the sound editor's Process menu (pvoc, sparkle~ cross)
// Fixed synthesis hop I = window / overlap, analysis hop D = I / stretch (so the synthesis overlap never thins out).
// Each parameter ramps from its first value to its second over the sound. The threshold is a fraction of the
// magnitude a sine at the sound's peak would have. other (otherN samples, read circularly): the second sound of a
// cross-synthesis (xmode 1-3). seed() gives the seeds of the whispering vocoders (phaseMode 2), when they are
// made. tick(0..1) returns false to stop: the result is then empty.
struct PvocOptions {
    double stretch0 = 1, stretch1 = 1, pitch0 = 1, pitch1 = 1, form0 = 1, form1 = 1, thr0 = 0, thr1 = 0, xa0 = 0, xa1 = 0;
    int window = 2048, overlap = 8, pad = 1, env = 0, xmode = 0, phaseMode = 0;
};
template <class T>
inline std::vector<double> pvoc(const T* x, size_t tot, const PvocOptions& o, const T* other, size_t otherN,
                                const std::function<uint32_t()>& seed, const std::function<bool(double)>& tick = {}) {
    if (tot == 0) return {};
    const bool cross = o.xmode != 0 && other && otherN > 0;
    Vocoder voc(o.window, o.pad);
    voc.envelope = o.env; voc.crossMode = cross ? o.xmode : 0; voc.phaseMode = o.phaseMode;
    if (voc.phaseMode == 2 && seed) voc.seed(seed());     // only whisper draws: the other random numbers stay where they were
    size_t Wn = (size_t)o.window, I = std::max<size_t>(1, Wn / (size_t)o.overlap);
    double D0 = (double)I / o.stretch0, D1 = (double)I / o.stretch1;
    size_t frames = (size_t)std::ceil((double)tot / ((D0 + D1) / 2));
    // the frames whose windows reach into the output from before its start or after its end: computed too (the
    // signal padded with a window of zeros on both sides), so the first window does not fade the attack in and the
    // last does not fade the end out; the output is then trimmed to the frames proper
    size_t edge = (Wn + I - 1) / I - 1;
    double peak = 0; for (size_t k = 0; k < tot; k++) peak = std::max(peak, std::fabs((double)x[k]));
    std::vector<double> out(I * (frames + 2 * edge) + Wn, 0.0), now(Wn), prev(Wn), onow(Wn), oprev(Wn), frame(Wn);
    auto fill = [&](const T* sig, size_t sn, long p, bool wrap, std::vector<double>& w) {
        long n = (long)sn;
        for (size_t k = 0; k < Wn; k++) {
            long idx = p + (long)k;
            if (wrap) { idx %= n; if (idx < 0) idx += n; w[k] = (double)sig[(size_t)idx]; }
            else w[k] = idx >= 0 && idx < n ? (double)sig[(size_t)idx] : 0.0;
        }
    };
    // one frame at the read position, synthesis hop Isyn, the parameters at t (0..1 over the frames), into frame
    auto run = [&](Vocoder& v, double read, double t, double Isyn) {
        double D = D0 + (D1 - D0) * t, P = o.pitch0 + (o.pitch1 - o.pitch0) * t, K = o.form0 + (o.form1 - o.form0) * t;
        double Th = o.thr0 + (o.thr1 - o.thr0) * t, A = o.xa0 + (o.xa1 - o.xa0) * t;
        long p = (long)std::floor(read), Dn = std::max(1L, std::lround(D));
        fill(x, tot, p, false, now); fill(x, tot, p - Dn, false, prev);
        if (cross) { fill(other, otherN, p, true, onow); fill(other, otherN, p - Dn, true, oprev); }
        v.process(now.data(), prev.data(), cross ? onow.data() : (const double*)nullptr, cross ? oprev.data() : (const double*)nullptr,
                  (double)Dn, Isyn, P, K, Th * peak, A, frame.data());
        return D;
    };
    double read = 0;
    for (size_t f = 0; f < frames + edge; f++) {          // from the start on, past the end by the edge frames
        double t = frames > 1 ? std::min(1.0, (double)f / (frames - 1)) : 0;
        read += run(voc, read, t, (double)I);
        for (size_t k = 0; k < Wn; k++) out[(edge + f) * I + k] += frame[k];
        if (tick && f % 64 == 0 && !tick((double)f / (double)(frames + edge))) return {};
    }
    if (edge > 0) {
        // before the start: a second vocoder walks back from the first frame (synthesis hop -I), so its phases are
        // the main one's continued back in time and the two overlap-add coherently. Same settings, same first frame
        // (its output is the main vocoder's: dropped); a negative hop negates the frame's scale, undone here
        Vocoder back(o.window, o.pad);
        back.envelope = voc.envelope; back.crossMode = voc.crossMode; back.phaseMode = voc.phaseMode;
        if (back.phaseMode == 2 && seed) back.seed(seed());
        run(back, 0, 0, (double)I);
        for (size_t k = 1; k <= edge; k++) {
            run(back, -(double)k * D0, 0, -(double)I);
            for (size_t j = 0; j < Wn; j++) out[(edge - k) * I + j] -= frame[j];
        }
    }
    return std::vector<double>(out.begin() + (long)(edge * I), out.begin() + (long)(edge * I + I * frames));
}

}  // namespace dsp
}  // namespace musil
