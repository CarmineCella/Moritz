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
//   - pitch shift by P, by resampling the spectrum;
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
#include <random>
#include <vector>

namespace musil {
namespace dsp {

class Vocoder {
public:
    Vocoder(int window, int pad) { init(window, pad); }

    // settings, read at every frame
    int envelope = 0;      // cepstral coefficients of the envelope (0: no formant preservation)
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
    // I: synthesis hop; P: pitch ratio; F: formant ratio; threshold: bins below threshold times a
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
        // pitch shift: the spectrum resampled by P
        if (P != 1) {
            for (size_t k = 0; k < N2; ++k) {
                double src = (double)k / P;
                bool in = src < (double)N2;
                size_t i = in ? (size_t)src : 0;
                newm_[k] = in ? mags_[i] : 0;
                newp_[k] = in ? psi_[i] : 0;
            }
            mags_.swap(newm_);
            psi_.swap(newp_);
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

}  // namespace dsp
}  // namespace musil
