// morphology.h — a sound's descriptors as curves over time, what a morphological orchestration follows (music.mu's
// target-analyse, Moritz's orchidea in its morphological mode) and what the adaptive segmentation cuts at: the frame
// spectra, the peaks of the spectral flux against its moving median, their density a second, the register (the
// centroid in MIDI pitch, the spread, the lowest strong partial) and the loudness. Musil's frame-spectra, flux-peaks,
// peak-density, register-curve, loudness-curve and moving-median are these functions. Self-contained apart from the
// FFT, the biquads, the IIR filter, the flux of mir.h and the running median of scientific/algorithms.h.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// Every function computes as the Musil it replaced did, operation for operation (the same sums in the same order, the
// same Hann window and FFT), so that what they return is what the interpreted versions returned.
#pragma once

#include "biquad.h"
#include "fft.h"
#include "iir.h"
#include "mir.h"
#include "../scientific/algorithms.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace musil {
namespace morph {

using spectra = std::vector<std::vector<double>>;

// Musil's (hann n): periodic, 0.5 - 0.5 cos(2 pi k / n)
inline std::vector<double> hann(size_t n) {
    std::vector<double> w(n);
    for (size_t k = 0; k < n; k++) w[k] = 0.5 - 0.5 * std::cos((dsp::kTwoPi * (double)k) / (double)n);
    return w;
}
// the magnitudes of Musil's (stft x n hop): frames at 0, hop, 2 hop ... until one reaches the end of x (the last padded
// with zeros), each Hann-windowed and transformed at the next power of two; the first keep bins of each
inline spectra stft_magnitudes(const std::vector<double>& x, size_t n, size_t hop, size_t keep) {
    if (n < 1 || hop < 1) throw std::runtime_error("stft: the block and the hop must be at least 1");
    std::vector<double> w = hann(n);
    double count = 1 + std::max(0.0, std::ceil(((double)x.size() - (double)n) / (double)hop));
    double want = (double)hop * (count - 1) + (double)n;
    size_t len = x.size() + (size_t)std::max(0.0, want - (double)x.size());
    size_t N = dsp::next_pow2(std::max<size_t>(2, n));
    keep = std::min(keep, N);
    spectra out;
    std::vector<double> buf(2 * N);
    for (size_t pos = 0; pos + n <= len; pos += hop) {
        std::fill(buf.begin(), buf.end(), 0.0);
        for (size_t k = 0; k < n; k++) buf[2 * k] = (pos + k < x.size() ? x[pos + k] : 0.0) * w[k];
        dsp::fft_inplace(buf.data(), N, -1);
        std::vector<double> m(keep);
        for (size_t k = 0; k < keep; k++) { double re = buf[2 * k], im = buf[2 * k + 1]; m[k] = std::sqrt(re * re + im * im); }
        out.push_back(std::move(m));
    }
    return out;
}
// (frame-spectra x block hop ncoeff): the magnitude spectrum of every frame (block/2 zeros before the sound, so that the
// first frame is centred on its start), the first ncoeff bins: the space of a database's spectrum features
inline spectra frame_spectra(const std::vector<double>& x, size_t block, size_t hop, size_t ncoeff) {
    std::vector<double> padded(block / 2, 0.0);
    padded.insert(padded.end(), x.begin(), x.end());
    return stft_magnitudes(padded, block, hop, ncoeff);
}
// (onset-strength x n hop): the spectral flux of every frame (0 for the first)
inline std::vector<double> onset_strength(const std::vector<double>& x, size_t n, size_t hop) {
    spectra mags = stft_magnitudes(x, n, hop, dsp::next_pow2(std::max<size_t>(2, n)) / 2);
    std::vector<double> flux(mags.empty() ? 0 : mags.size(), 0.0);
    for (size_t k = 1; k < mags.size(); k++) flux[k] = mir::spectralFlux(mags[k], mags[k - 1]);
    return flux;
}
// (flux-peaks x sr n hop threshold width): the times (s) of the peaks of the spectral flux above threshold times its
// moving median over width frames (plus a hundredth of its maximum), 50 ms apart at least
inline std::vector<double> flux_peaks(const std::vector<double>& x, double sr, size_t n, size_t hop, double threshold, long width) {
    std::vector<double> flux = onset_strength(x, n, hop), out;
    if (flux.size() < 3) return out;
    if (width < 1) throw std::runtime_error("flux-peaks: the width must be >= 1");
    double mx = *std::max_element(flux.begin(), flux.end());
    std::vector<double> med = median_filter(flux.data(), (long)flux.size(), width);
    double last = -1, frame = (double)hop / sr;
    for (size_t k = 1; k + 1 < flux.size(); k++) {
        if (!(flux[k] > flux[k - 1] && flux[k] > flux[k + 1])) continue;
        if (!(flux[k] > threshold * (med[k] + 0.01 * mx))) continue;
        double t = (double)k * frame;
        if (t - last >= 0.05) { out.push_back(t); last = t; }
    }
    return out;
}
// (peak-density times secs window hop): how many of the times fall within window seconds around each step of hop
// seconds over secs, divided by the window (events a second)
inline std::vector<double> peak_density(const std::vector<double>& times, double secs, double window, double hop) {
    if (!(hop > 0)) throw std::runtime_error("peak-density: the hop must be > 0");
    double steps = std::ceil(secs / hop);
    std::vector<double> out;
    for (double k = 0; k < steps; k += 1) {
        double t = k * hop; int n = 0;
        for (double o : times) if (std::fabs(o - t) < window / 2) n++;
        out.push_back((double)n / window);
    }
    return out;
}
// (register-curve spectra sr block): per frame the centroid in MIDI pitch (the powers above 30 Hz weighting each bin's
// pitch), the spread around it in octaves and the lowest strong partial; a frame below -60 dB keeps the values before
struct register_curves { std::vector<double> centroid, spread, low; };
inline register_curves register_curve(const spectra& frames, double sr, double block) {
    register_curves r;
    if (frames.empty()) throw std::runtime_error("register-curve: no frames");
    size_t n = frames[0].size();
    std::vector<double> freqs(n), keep(n), pitches(n);
    for (size_t k = 0; k < n; k++) {
        freqs[k] = (double)k * (sr / block);
        keep[k] = freqs[k] > 30 ? 1 : 0;
        pitches[k] = keep[k] * (69 + 12 * std::log2(std::max(freqs[k], 30.0) / 440));
    }
    double floor = 1e-3 * (block / 4), c = 60, s = 1, low = 48;
    for (const auto& m : frames) {
        if (m.size() != n) throw std::runtime_error("register-curve: the frames differ in length");
        double mx = n ? *std::max_element(m.begin(), m.end()) : 0;
        if (mx > floor) {
            std::vector<double> w(n); double sw = 0;
            for (size_t k = 0; k < n; k++) { w[k] = (m[k] * m[k]) * keep[k]; sw += w[k]; }
            if (sw > 1e-12) {
                double d = 0; for (size_t k = 0; k < n; k++) d += w[k] * pitches[k];
                c = d / sw;
                double v = 0; for (size_t k = 0; k < n; k++) { double e = pitches[k] - c; v += w[k] * (e * e); }
                s = std::sqrt(v / sw) / 12;
            }
            for (size_t k = 1; k + 1 < n; k++)
                if (m[k] > m[k - 1] && m[k] > m[k + 1] && freqs[k] > 30 && m[k] > 0.15 * mx) { low = pitches[k]; break; }
        }
        r.centroid.push_back(c); r.spread.push_back(s); r.low.push_back(low);
    }
    return r;
}
// (loudness-curve x sr hop): the level of every hop in dB (-100 for silence), the extreme bass and treble taken out
// (a highpass at 150 Hz and a lowpass at 8 kHz, as Musil's highpass and lowpass)
inline std::vector<double> loudness_curve(const std::vector<double>& x, double sr, size_t hop) {
    if (hop < 1) throw std::runtime_error("loudness-curve: the hop must be at least 1");
    std::vector<double> a(x.size()), w(x.size());
    auto run = [](const std::vector<double>& in, std::vector<double>& out, const dsp::BiquadCoeffs& c) {
        double b[3] = {c.b0, c.b1, c.b2}, av[3] = {c.a0, c.a1, c.a2}; iir_state st;
        if (!in.empty()) iir_run(in.data(), out.data(), in.size(), b, 3, av, 3, st);
    };
    run(x, a, dsp::rbj(dsp::BiquadType::Highpass, sr, 150, 0.7, 0));
    double f = 0.45 * sr; if (8000 < f) f = 8000;
    run(a, w, dsp::rbj(dsp::BiquadType::Lowpass, sr, f, 0.7, 0));
    double count = std::ceil((double)w.size() / (double)hop);
    std::vector<double> out;
    for (double k = 0; k < count; k += 1) {
        size_t start = (size_t)(k * (double)hop), len = std::min(hop, w.size() - std::min(w.size(), start));
        if (len == 0) { out.push_back(-100); continue; }
        double s = 0; for (size_t j = 0; j < len; j++) s += w[start + j] * w[start + j];
        double r = std::sqrt(s / (double)len) + 1e-6;   // (+ 1e-6 (rms seg)): the same sum either way round
        double db = 20 * std::log10(std::max(r, 1e-12));
        out.push_back(std::max(-100.0, db));
    }
    return out;
}
// (moving-median v n): each value the median of the n around it (fewer at the ends)
inline std::vector<double> moving_median(const std::vector<double>& v, long n) {
    long h = n / 2, len = (long)v.size();
    std::vector<double> out((size_t)len);
    for (long k = 0; k < len; k++) {
        long lo = std::max(0L, k - h), cnt = std::min(len, k + h + 1) - lo;
        std::vector<double> s(v.begin() + lo, v.begin() + lo + cnt);
        std::sort(s.begin(), s.end());
        out[(size_t)k] = cnt % 2 ? s[(size_t)(cnt / 2)] : (s[(size_t)(cnt / 2 - 1)] + s[(size_t)(cnt / 2)]) / 2;
    }
    return out;
}

}  // namespace morph
}  // namespace musil
