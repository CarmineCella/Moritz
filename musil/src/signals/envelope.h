// envelope.h — the spectral envelope of Musil's spectral-envelope (and of flatten-spectrum, impose-envelope),
// for Moritz's specenv~ too. Self-contained (the standard library and fft.h).
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// The "true envelope" (Imai and Abe; Roebel and Rodet): the log magnitude spectrum smoothed by keeping its first
// `order` cepstral coefficients, then smoothed again, `iterations` times, from the maximum of the spectrum and the
// last envelope, so that it rides on the harmonic peaks instead of averaging peaks and valleys (0 iterations: the
// plain cepstral smoothing). Magnitudes under 1e-4 of the frame's peak count as that floor.
//
// The order: the cepstral coefficients kept, a quefrency in samples whatever the FFT size. About sr / (2 f0), f0
// the sound's (lowest) fundamental, follows the formants without rippling at the harmonics: 150 for a voice at
// 44.1 kHz, 50 at 16 kHz; a lower order is smoother. The phase vocoder's envelope (vocoder.h, pvoc's 'envelope)
// takes its order in the same unit and the same advice holds; it rides the peaks by interpolating between
// them before one smoothing, so it tolerates a somewhat higher order.
#pragma once

#include "fft.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace musil {
namespace dsp {

template <class T>
class TrueEnvelope {
public:
    // the size of the spectra (the whole circle, a power of two >= 4)
    void init(int M) {
        if (M == M_) return;
        M_ = M;
        fft_.init(M);
        target_.assign((size_t)M, (T)0);
        cur_.assign((size_t)M, (T)0);
        work_.assign((size_t)M, (T)0);
        cr_.assign((size_t)M / 2 + 1, (T)0);
        ci_.assign((size_t)M / 2 + 1, (T)0);
    }
    int size() const { return M_; }
    // mags: M magnitudes, the whole (symmetric) spectrum; env: M values
    void full(const T* mags, int order, int iterations, T* env) {
        T peak = 0;
        for (int k = 0; k < M_; ++k) peak = std::max(peak, mags[k]);
        T fl = std::max(peak * (T)1e-4, std::numeric_limits<T>::min());   // 1e-300 is 0 in float
        for (int k = 0; k < M_; ++k) target_[(size_t)k] = std::log(std::max(mags[k], fl));
        run(order, iterations);
        for (int k = 0; k < M_; ++k) env[k] = std::exp(cur_[(size_t)k]);
    }
    // amp: bins magnitudes of the positive frequencies (bins <= M/2 + 1), mirrored onto the M points; env: bins values
    void half(const T* amp, int bins, int order, int iterations, T* env) {
        T peak = 0;
        for (int k = 0; k < bins; ++k) peak = std::max(peak, amp[k]);
        T fl = std::max(peak * (T)1e-4, (T)1e-12);
        for (int k = 0; k < M_; ++k) {
            int b = std::min(k <= M_ / 2 ? k : M_ - k, bins - 1);
            target_[(size_t)k] = std::log(std::max(amp[b], fl));
        }
        run(order, iterations);
        for (int k = 0; k < bins; ++k) env[k] = std::exp(cur_[(size_t)k]);
    }

private:
    void run(int order, int iterations) {
        order = std::max(0, std::min(order, M_ / 2 - 1));
        smooth(target_.data(), order);
        for (int it = 0; it < iterations; ++it) {
            for (int k = 0; k < M_; ++k) work_[(size_t)k] = std::max(target_[(size_t)k], cur_[(size_t)k]);
            smooth(work_.data(), order);
        }
    }
    // cur = the log spectrum in with only its first `order` cepstral coefficients (a real, even sequence)
    void smooth(const T* in, int order) {
        fft_.forward(in, cr_.data(), ci_.data());          // the real cepstrum, times M (in is even: real)
        for (int k = order + 1; k <= M_ / 2; ++k) cr_[(size_t)k] = 0;
        std::fill(ci_.begin(), ci_.end(), (T)0);
        fft_.inverse(cr_.data(), ci_.data(), cur_.data());   // forward then inverse, unscaled: M times
        for (auto& v : cur_) v /= (T)M_;
    }

    int M_ = 0;
    RealFFT<T> fft_;
    std::vector<T> target_, cur_, work_, cr_, ci_;
};

}  // namespace dsp
}  // namespace musil
