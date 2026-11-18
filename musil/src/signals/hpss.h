// hpss.h — harmonic / percussive separation (Fitzgerald 2010): Musil's hpss, and Moritz's hpss object.
// Self-contained (the standard library and fft.h).
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// STFT with a periodic Hann window of n samples (a power of two) every hop; the magnitude spectrogram is
// median-filtered along time (harmonic sounds are horizontal lines) and along frequency (percussive sounds are
// vertical ones), kernel frames and bins (odd, ~17); soft masks from the two filtered spectrograms (Wiener-like,
// squared) weight the original spectra, which are inverted and overlap-added. The signal is padded with n zeros on
// both sides, so its edges are covered by full overlaps, and the overlap-added squared window is divided out: the
// two parts are as long as the input and add up to it. Computed in double whatever T is.
#pragma once

#include "fft.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace musil {
namespace dsp {

// x: len samples; harm, perc: len samples out. breathe (optional) is called now and then (a host keeps its
// windows alive); it returns false to stop (the outputs are then left empty)
template <class T>
inline bool hpss(const T* x, size_t len, size_t n, size_t hop, int kernel, std::vector<T>& harm, std::vector<T>& perc,
                 const std::function<bool()>& breathe = {}) {
    harm.assign(len, (T)0);
    perc.assign(len, (T)0);
    if (len == 0 || n < 4 || hop < 1) return true;
    if (kernel < 3) kernel = 3;
    if (kernel % 2 == 0) kernel++;
    std::vector<double> xp(len + 2 * n, 0.0);
    for (size_t k = 0; k < len; k++) xp[n + k] = (double)x[k];
    size_t half = (size_t)kernel / 2, bins = n / 2 + 1, frames = (xp.size() - n) / hop + 1;
    std::vector<double> w(n);
    for (size_t k = 0; k < n; k++) w[k] = 0.5 - 0.5 * std::cos(kTwoPi * (double)k / (double)n);
    RealFFT<double> fft;
    fft.init((int)n);
    // analysis
    std::vector<std::vector<double>> re(frames, std::vector<double>(bins)), im(frames, std::vector<double>(bins)), mag(frames, std::vector<double>(bins));
    std::vector<double> buf(n);
    for (size_t f = 0; f < frames; f++) {
        for (size_t k = 0; k < n; k++) { size_t idx = f * hop + k; buf[k] = idx < xp.size() ? xp[idx] * w[k] : 0; }
        fft.forward(buf.data(), re[f].data(), im[f].data());
        for (size_t b = 0; b < bins; b++) mag[f][b] = std::hypot(re[f][b], im[f][b]);
        if (breathe && f % 256 == 255 && !breathe()) { harm.clear(); perc.clear(); return false; }
    }
    auto median = [](std::vector<double>& v) { size_t m = v.size() / 2; std::nth_element(v.begin(), v.begin() + (long)m, v.end()); return v[m]; };
    // medians along time (harmonic) and along frequency (percussive)
    std::vector<std::vector<double>> H(frames, std::vector<double>(bins)), P(frames, std::vector<double>(bins));
    std::vector<double> win;
    win.reserve((size_t)kernel);
    for (size_t b = 0; b < bins; b++) for (size_t f = 0; f < frames; f++) {
        win.clear();
        for (long t = (long)f - (long)half; t <= (long)f + (long)half; t++) win.push_back(t >= 0 && t < (long)frames ? mag[(size_t)t][b] : 0.0);
        H[f][b] = median(win);
    }
    for (size_t f = 0; f < frames; f++) for (size_t b = 0; b < bins; b++) {
        win.clear();
        for (long q = (long)b - (long)half; q <= (long)b + (long)half; q++) win.push_back(q >= 0 && q < (long)bins ? mag[f][(size_t)q] : 0.0);
        P[f][b] = median(win);
    }
    if (breathe && !breathe()) { harm.clear(); perc.clear(); return false; }
    // soft masks, synthesis
    size_t outLen = (frames - 1) * hop + n;
    std::vector<double> ho(outLen, 0.0), po(outLen, 0.0), wsum(outLen, 0.0);    // wsum: the overlap-added squared window
    for (size_t f = 0; f < frames; f++) for (size_t k = 0; k < n; k++) wsum[f * hop + k] += w[k] * w[k];
    std::vector<double> hr(bins), hi(bins), pr(bins), pi(bins), th(n), tp(n);
    for (size_t f = 0; f < frames; f++) {
        for (size_t b = 0; b < bins; b++) {
            double h2 = H[f][b] * H[f][b], p2 = P[f][b] * P[f][b], d = h2 + p2 + 1e-12;
            double mh = h2 / d, mp = p2 / d;
            hr[b] = re[f][b] * mh; hi[b] = im[f][b] * mh; pr[b] = re[f][b] * mp; pi[b] = im[f][b] * mp;
        }
        fft.inverse(hr.data(), hi.data(), th.data());
        fft.inverse(pr.data(), pi.data(), tp.data());
        for (size_t k = 0; k < n; k++) { ho[f * hop + k] += th[k] / (double)n * w[k]; po[f * hop + k] += tp[k] / (double)n * w[k]; }
    }
    for (size_t k = 0; k < len && n + k < outLen; k++) {           // the padding removed: as long as the input
        double g = std::max(wsum[n + k], 1e-3);
        harm[k] = (T)(ho[n + k] / g);
        perc[k] = (T)(po[n + k] / g);
    }
    return true;
}

}  // namespace dsp
}  // namespace musil
