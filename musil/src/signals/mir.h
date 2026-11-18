// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// mir.h — music information retrieval in C++, frame by frame and over whole sounds: spectrograms, mel bands
// and MFCCs, chroma, spectral flux and onsets, tempo and beats, keys, self-similarity and novelty, loudness.
// Maple and SoundTypes (music/maple.h, music/soundtypes.h) are built on it, and a host can use it directly
// (Moritz's analysis objects, offline and live, do). It needs nothing but the standard library.
//
// A spectrogram has a frame every hop samples; frame f is centred on sample
// f * hop (the signal is padded with n/2 zeros on both sides), so its time
// is f * hop / sr. Amplitudes are scaled so that a full-scale sine gives 1
// (0 dB). A matrix is a list of rows (features), each with one value per
// frame.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "fft.h"

namespace musil {
namespace mir {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// the FFT on floats of the signal engines (fft.h): unscaled inverse, inverse(forward(x)) == n * x
using FFT = dsp::FFT<float>;

using Vec = std::vector<double>;
using Mat = std::vector<Vec>;              // rows x frames (or frames x bins for a spectrogram)
using Tick = std::function<bool(double)>;  // progress 0..1; returns false to stop

constexpr double kFloorDb = -100.0;

inline double toDb(double power) { return std::max(kFloorDb, 10.0 * std::log10(power + 1e-30)); }

// ------------------------------------------------------------------ spectra
// Magnitudes of one frame (n samples, already windowed or not): n/2 + 1 bins.
class FrameSpectrum {
public:
    void init(int n) {
        n_ = n;
        fft_.init(n);
        re_.assign((size_t)n, 0.f);
        im_.assign((size_t)n, 0.f);
        win_.resize((size_t)n);
        for (int k = 0; k < n; ++k) win_[(size_t)k] = (float)(.5 - .5 * std::cos(kTwoPi * k / n));
    }
    int size() const { return n_; }
    // x: n samples; mag gets n/2 + 1 amplitudes (a full-scale sine = 1)
    void magnitudes(const float* x, Vec& mag) {
        for (int k = 0; k < n_; ++k) { re_[(size_t)k] = x[k] * win_[(size_t)k]; im_[(size_t)k] = 0.f; }
        fft_.forward(re_.data(), im_.data());
        mag.resize((size_t)n_ / 2 + 1);
        double scale = 4.0 / n_;
        for (int b = 0; b <= n_ / 2; ++b) mag[(size_t)b] = std::hypot(re_[(size_t)b], im_[(size_t)b]) * scale;
    }

private:
    int n_ = 0;
    FFT fft_;
    std::vector<float> re_, im_, win_;
};

inline int frameCount(long length, int hop) { return (int)(length / hop) + 1; }

// frames x (n/2 + 1) magnitudes of a mono signal
inline Mat magSpectrogram(const std::vector<float>& x, int n, int hop, const Tick& tick = {}) {
    int frames = frameCount((long)x.size(), hop);
    Mat mag((size_t)frames);
    FrameSpectrum fs;
    fs.init(n);
    std::vector<float> buf((size_t)n);
    for (int f = 0; f < frames; ++f) {
        long start = (long)f * hop - n / 2;
        for (int k = 0; k < n; ++k) {
            long i = start + k;
            buf[(size_t)k] = (i >= 0 && i < (long)x.size()) ? x[(size_t)i] : 0.f;
        }
        fs.magnitudes(buf.data(), mag[(size_t)f]);
        if (tick && f % 64 == 0 && !tick((double)f / frames)) return {};
    }
    return mag;
}

// ------------------------------------------------------------------ mel, MFCC, chroma
inline double hzToMel(double f) { return 2595.0 * std::log10(1 + f / 700.0); }
inline double melToHz(double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1); }

// bands triangular filters evenly spaced on the mel scale from 0 to sr/2
class MelBank {
public:
    void init(double sr, int n, int bands) {
        sr_ = sr; n_ = n; bands_ = bands;
        int bins = n / 2 + 1;
        double hi = hzToMel(sr / 2);
        Vec edges((size_t)bands + 2);
        for (int k = 0; k < bands + 2; ++k) edges[(size_t)k] = melToHz(hi * k / (bands + 1));
        lo_.assign((size_t)bands, 0);
        w_.assign((size_t)bands, Vec());
        for (int m = 0; m < bands; ++m) {
            double l = edges[(size_t)m], c = edges[(size_t)m + 1], r = edges[(size_t)m + 2];
            int first = -1;
            Vec w;
            for (int b = 0; b < bins; ++b) {
                double f = b * sr / n, v = 0;
                if (f > l && f < c) v = (f - l) / (c - l);
                else if (f >= c && f < r) v = (r - f) / (r - c);
                if (v > 0) { if (first < 0) first = b; w.resize((size_t)(b - first + 1), 0.0); w[(size_t)(b - first)] = v; }
            }
            if (first < 0) {                       // narrower than a bin: the nearest bin
                first = std::clamp((int)std::lround(c * n / sr), 0, bins - 1);
                w = {1.0};
            }
            lo_[(size_t)m] = first;
            w_[(size_t)m] = std::move(w);
        }
    }
    bool matches(double sr, int n, int bands) const { return sr == sr_ && n == n_ && bands == bands_; }
    int bands() const { return bands_; }
    // the energy of every band in dB
    void apply(const Vec& mag, Vec& out) const {
        out.resize((size_t)bands_);
        for (int m = 0; m < bands_; ++m) {
            double e = 0;
            const Vec& w = w_[(size_t)m];
            for (size_t j = 0; j < w.size(); ++j) {
                double a = mag[(size_t)lo_[(size_t)m] + j];
                e += w[j] * a * a;
            }
            out[(size_t)m] = toDb(e);
        }
    }

private:
    double sr_ = 0;
    int n_ = 0, bands_ = 0;
    std::vector<int> lo_;
    std::vector<Vec> w_;
};

// the first ncoeff coefficients of the DCT-II (orthonormal) of log-mel bands
inline void dct(const Vec& in, int ncoeff, Vec& out) {
    int M = (int)in.size();
    out.assign((size_t)ncoeff, 0.0);
    for (int c = 0; c < ncoeff; ++c) {
        double s = 0;
        for (int m = 0; m < M; ++m) s += in[(size_t)m] * std::cos(kPi * c * (m + .5) / M);
        out[(size_t)c] = s * std::sqrt(2.0 / M) * (c == 0 ? std::sqrt(.5) : 1.0);
    }
}

// The pitch classes of a spectrum are read from its peaks (as HPCP): the
// local maxima, their frequency refined by a parabola through the peak and
// its neighbours (on the log magnitudes), each given to the nearest note
// between C2 and B6, so a tone counts once. Reading the smooth spectrum
// instead would let the window's main lobe (four bins: several semitones in
// the bass) spill every note onto its neighbours.
struct ChromaMap {
    double sr = 0;
    int n = 0, midiLo = 36, midiHi = 95;
    bool matches(double rate, int size) const { return sr == rate && n == size; }
    void init(double rate, int size) {
        sr = rate;
        n = size;
    }
};
inline ChromaMap chromaMap(double sr, int n) {
    ChromaMap c;
    c.init(sr, n);
    return c;
}
// The 12 pitch classes (C first) of a frame: every peak above -80 dB and
// above 1/1000 of the frame's strongest adds its magnitude to the pitch
// class of its nearest note; a peak that is probably the 3rd, 5th, 6th or
// 7th partial of a lower peak (one at that fraction of its frequency, within
// a third of a semitone, at least 0.3 as loud) counts a tenth, since it would
// add another pitch class (the fifth, the third...): without this, brass
// and strings pull the key a fifth up. Octave partials keep their class.
// The result is scaled to a maximum of 1 (silence stays 0).
inline void chroma(const Vec& mag, const ChromaMap& map, Vec& out) {
    out.assign(12, 0.0);
    const double kPartial = .1, kRel = .3;
    int bins = (int)mag.size();
    double mx = 0;
    for (double v : mag) mx = std::max(mx, v);
    if (mx < 1e-4) return;                                     // below -80 dB: silence
    double floor = std::max(1e-4, mx * 1e-3);
    std::vector<std::pair<double, double>> peaks;             // frequency, magnitude
    for (int b = 2; b + 1 < bins; ++b) {
        double m = mag[(size_t)b];
        if (m < floor || m <= mag[(size_t)b - 1] || m < mag[(size_t)b + 1]) continue;
        double l = std::log(mag[(size_t)b - 1] + 1e-12), c = std::log(m + 1e-12), r = std::log(mag[(size_t)b + 1] + 1e-12);
        double d = l - 2 * c + r, off = d < 0 ? std::clamp(.5 * (l - r) / d, -.5, .5) : 0.0;
        peaks.push_back({(b + off) * map.sr / map.n, m});
    }
    for (size_t i = 0; i < peaks.size(); ++i) {
        double f = peaks[i].first, m = peaks[i].second;
        int note = (int)std::lround(69 + 12 * std::log2(f / 440.0));
        if (note < map.midiLo || note > map.midiHi) continue;
        double w = 1.0;
        for (int h : {3, 5, 6, 7}) {
            double f0 = f / h;
            for (size_t j = 0; j < i && w == 1.0; ++j)       // the peaks below
                if (std::fabs(12 * std::log2(peaks[j].first / f0)) < .33 && peaks[j].second >= kRel * m) w = kPartial;
        }
        out[(size_t)(note % 12)] += w * m;
    }
    double top = *std::max_element(out.begin(), out.end());
    if (top > 0)
        for (auto& v : out) v /= top;
}

// rectified increase of the magnitudes since the previous frame
inline double spectralFlux(const Vec& mag, const Vec& prev) {
    double s = 0;
    for (size_t b = 0; b < mag.size() && b < prev.size(); ++b) s += std::max(0.0, mag[b] - prev[b]);
    return s;
}
inline Vec fluxOf(const Mat& mag) {
    Vec flux(mag.size(), 0.0);
    for (size_t f = 1; f < mag.size(); ++f) flux[f] = spectralFlux(mag[f], mag[f - 1]);
    return flux;
}

// ------------------------------------------------------------------ helpers
inline Vec medianFilter(const Vec& v, int width) {
    int h = std::max(0, width / 2), n = (int)v.size();
    Vec out(v.size()), buf;
    for (int k = 0; k < n; ++k) {
        buf.clear();
        for (int j = k - h; j <= k + h; ++j)
            if (j >= 0 && j < n) buf.push_back(v[(size_t)j]);
        size_t m = buf.size() / 2;
        std::nth_element(buf.begin(), buf.begin() + (long)m, buf.end());
        out[(size_t)k] = buf[m];
    }
    return out;
}
inline std::vector<int> localMaxima(const Vec& v) {
    std::vector<int> out;
    for (size_t k = 1; k + 1 < v.size(); ++k)
        if (v[k] > v[k - 1] && v[k] >= v[k + 1]) out.push_back((int)k);
    return out;
}
inline double maxOf(const Vec& v) { return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()); }
inline double meanOf(const Vec& v) {
    double s = 0;
    for (double x : v) s += x;
    return v.empty() ? 0.0 : s / (double)v.size();
}
inline double correlation(const Vec& a, const Vec& b) {
    double ma = meanOf(a), mb = meanOf(b), sab = 0, saa = 0, sbb = 0;
    for (size_t k = 0; k < a.size() && k < b.size(); ++k) {
        double da = a[k] - ma, db = b[k] - mb;
        sab += da * db; saa += da * da; sbb += db * db;
    }
    double d = std::sqrt(saa * sbb);
    return d > 0 ? sab / d : 0.0;
}

// ------------------------------------------------------------------ onsets
// The sample within [start, start + n) where the energy rises most (64-sample
// blocks): a hit is placed where it starts, not a window early.
inline long onsetRefine(const std::vector<float>& x, long start, int n) {
    const int blk = 64;
    start = std::max(0L, start);
    long len = std::min((long)n, (long)x.size() - start);
    if (len < 2 * blk) return start;
    int blocks = (int)(len / blk);
    Vec e((size_t)blocks);
    for (int j = 0; j < blocks; ++j) {
        double s = 0;
        for (int k = 0; k < blk; ++k) { double v = x[(size_t)(start + (long)j * blk + k)]; s += v * v; }
        e[(size_t)j] = std::sqrt(s / blk);
    }
    int best = -1;
    double bd = 0;
    for (int j = 0; j + 1 < blocks; ++j)
        if (e[(size_t)j + 1] - e[(size_t)j] > bd) { bd = e[(size_t)j + 1] - e[(size_t)j]; best = j; }
    return best < 0 ? start : start + (long)blk * (best + 1);
}
// Onset times in seconds: the peaks of the flux above threshold times its
// moving median over width frames (plus 1% of its maximum), at least `gap`
// seconds apart, each refined to the sample where the energy jumps.
inline Vec onsetTimes(const std::vector<float>& x, double sr, int n, int hop, const Vec& flux, double threshold,
                      int width, double gap) {
    Vec out;
    if (flux.size() < 3) return out;
    Vec med = medianFilter(flux, width);
    double floor = .01 * maxOf(flux);
    long last = -1000000000L;
    for (int k : localMaxima(flux)) {
        if (flux[(size_t)k] <= threshold * (med[(size_t)k] + floor)) continue;
        long s = onsetRefine(x, (long)k * hop - n / 2, n);
        if ((double)(s - last) / sr >= gap) {
            out.push_back((double)s / sr);
            last = s;
        }
    }
    return out;
}

// ------------------------------------------------------------------ tempo and beats
// biased autocorrelation (sum / n) for lags 0 .. maxLag
inline Vec autocorr(const Vec& x, int maxLag) {
    int n = (int)x.size();
    Vec ac((size_t)std::max(0, maxLag + 1), 0.0);
    for (int lag = 0; lag <= maxLag && lag < n; ++lag) {
        double s = 0;
        for (int k = 0; k + lag < n; ++k) s += x[(size_t)k] * x[(size_t)k + lag];
        ac[(size_t)lag] = s / n;
    }
    return ac;
}
// The tempo of an onset strength (fps values a second): its autocorrelation
// read at the lags of 30..300 bpm, weighted by a log-normal prior on the
// tempo a listener taps, centred on `centre` bpm, `width` octaves wide (the
// pulse of the eighths or of the bars is often as periodic as the beat's:
// the prior decides between the octaves).
inline double tempoEstimate(const Vec& flux, double fps, double centre = 100, double width = .6) {
    Vec c(flux);
    double m = meanOf(c);
    for (auto& v : c) v -= m;
    Vec ac = autocorr(c, (int)std::ceil(fps * 60 / 30) + 2);
    double best = 0, bestBpm = centre;
    for (double bpm = 30; bpm <= 300; bpm += .5) {
        double lag = fps * 60 / bpm;
        int l0 = (int)lag;
        if (l0 + 1 >= (int)ac.size()) continue;
        double t = lag - l0, v = ac[(size_t)l0] * (1 - t) + ac[(size_t)l0 + 1] * t;
        double w = std::exp(-.5 * std::pow(std::log2(bpm / centre) / width, 2));
        if (v * w > best) { best = v * w; bestBpm = bpm; }
    }
    return bestBpm;
}
// The frames of the beats: dynamic programming over the onset strength for
// the sequence of beats that is strong and keeps near the period of bpm
// (tightness ~100: how much a deviation from the period costs; Ellis 2007).
inline std::vector<int> beatTrack(const Vec& o, double fps, double bpm, double tight = 100) {
    int n = (int)o.size();
    std::vector<int> beats;
    if (n < 2 || bpm <= 0) return beats;
    double period = fps * 60 / bpm;
    int lo = std::max(1, (int)std::lround(period / 2)), hi = std::max(lo, (int)std::lround(period * 2));
    double mx = maxOf(o), scale = mx > 0 ? 1 / mx : 1;
    Vec score((size_t)n, 0.0);
    std::vector<int> from((size_t)n, -1);
    for (int t = 0; t < n; ++t) {
        double best = 0;
        int arg = -1;
        for (int lag = lo; lag <= hi && lag <= t; ++lag) {
            double d = std::log(lag / period), c = score[(size_t)(t - lag)] - tight * d * d;
            if (arg < 0 || c > best) { best = c; arg = t - lag; }
        }
        score[(size_t)t] = o[(size_t)t] * scale + (arg >= 0 ? best : 0);
        from[(size_t)t] = arg;
    }
    int last = n - 1;
    double bs = -1e300;
    for (int t = std::max(0, n - hi); t < n; ++t)
        if (score[(size_t)t] > bs) { bs = score[(size_t)t]; last = t; }
    for (int t = last; t >= 0; t = from[(size_t)t]) {
        beats.push_back(t);
        if (from[(size_t)t] < 0) break;
    }
    std::reverse(beats.begin(), beats.end());
    return beats;
}
// tempogram: every `step` frames, the autocorrelation of the onset strength
// (above its local mean) over a Hann window of `win` frames around it, read at 30..300 bpm (one row
// per bpm, row 0 = 30 bpm), divided by the autocorrelation at lag 0 (the
// window's energy), so 1 is a perfectly periodic pulse and a passage without
// one stays dark. Its columns come fps / step a second.
inline Mat tempogram(const Vec& flux, double fps, int win, int step) {
    // the flux above its local mean (0.3 s), rectified: the peaks, without
    // the slow decay of a big hit (which would look periodic at every tempo)
    Vec o(flux.size(), 0.0);
    {
        int r = std::max(1, (int)std::lround(fps * .15));
        for (long i = 0; i < (long)flux.size(); ++i) {
            double m = 0;
            int cnt = 0;
            for (long j = i - r; j <= i + r; ++j)
                if (j >= 0 && j < (long)flux.size()) { m += flux[(size_t)j]; ++cnt; }
            o[(size_t)i] = std::max(0.0, flux[(size_t)i] - m / cnt);
        }
    }
    win = std::max(8, win + (win & 1));
    step = std::max(1, step);
    int frames = (int)o.size(), h = win / 2, cols = (frames + step - 1) / step;
    int maxLag = std::min(h - 1, (int)std::ceil(fps * 60 / 30) + 1);
    Vec w((size_t)win), seg((size_t)win), ac((size_t)maxLag + 1);
    for (int k = 0; k < win; ++k) w[(size_t)k] = .5 - .5 * std::cos(kTwoPi * k / win);
    Mat rows(271, Vec((size_t)cols, 0.0));
    for (int col = 0; col < cols; ++col) {
        int f = col * step;
        double m = 0;
        int cnt = 0;
        for (int k = 0; k < win; ++k) {
            long i = (long)f - h + k;
            if (i >= 0 && i < frames) { m += o[(size_t)i]; ++cnt; }
        }
        m = cnt ? m / cnt : 0;
        for (int k = 0; k < win; ++k) {
            long i = (long)f - h + k;
            seg[(size_t)k] = (i >= 0 && i < frames) ? (o[(size_t)i] - m) * w[(size_t)k] : 0;
        }
        for (int lag = 0; lag <= maxLag; ++lag) {
            double s = 0;
            for (int k = 0; k + lag < win; ++k) s += seg[(size_t)k] * seg[(size_t)(k + lag)];
            ac[(size_t)lag] = std::max(0.0, s);
        }
        double e = ac[0] > 1e-12 ? ac[0] : 1.0;
        for (int bpm = 30; bpm <= 300; ++bpm) {
            double lag = fps * 60 / bpm, v = 0;
            int l0 = (int)lag;
            double t = lag - l0;
            if (l0 + 1 <= maxLag) v = ac[(size_t)l0] * (1 - t) + ac[(size_t)l0 + 1] * t;
            else if (l0 <= maxLag) v = ac[(size_t)l0];
            rows[(size_t)(bpm - 30)][(size_t)col] = v / e;
        }
    }
    return rows;
}

// ------------------------------------------------------------------ key
// the tonic of a key as musicians usually spell it (Eb major, C# minor)
inline const char* keyName(int k, bool minor = false) {
    static const char* const major[12] = {"C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    static const char* const minorNames[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B"};
    return (minor ? minorNames : major)[((k % 12) + 12) % 12];
}
// Krumhansl & Kessler's profiles (probe-tone experiments, Krumhansl 1990)
inline const std::array<double, 12>& majorProfile() {
    static const std::array<double, 12> p = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
    return p;
}
inline const std::array<double, 12>& minorProfile() {
    static const std::array<double, 12> p = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
    return p;
}
// the correlation of a pitch-class distribution (12 values, C first) with
// every key: 24 values, C major, C minor, C# major, ... B minor
inline Vec keyScores(const Vec& pcd) {
    Vec out;
    for (int k = 0; k < 12; ++k)
        for (int minor = 0; minor < 2; ++minor) {
            const auto& p = minor ? minorProfile() : majorProfile();
            Vec rot(12);
            for (int j = 0; j < 12; ++j) rot[(size_t)j] = p[(size_t)(((j - k) % 12 + 12) % 12)];
            out.push_back(correlation(pcd, rot));
        }
    return out;
}

// ------------------------------------------------------------------ structure
// The frames x frames cosine similarity of the columns of a feature matrix
// (rows features, columns frames): 1 on the diagonal, 0 for frames with
// nothing in common (Foote 1999: repeats are stripes off the diagonal).
inline Mat selfSimilarity(const Mat& F, const Tick& tick = {}) {
    size_t rows = F.size(), frames = rows ? F[0].size() : 0;
    Vec norm(frames, 0.0);
    for (size_t f = 0; f < frames; ++f) {
        double s = 0;
        for (size_t r = 0; r < rows; ++r) s += F[r][f] * F[r][f];
        norm[f] = std::sqrt(s);
    }
    Mat S(frames, Vec(frames, 0.0));
    for (size_t p = 0; p < frames; ++p) {
        for (size_t q = p; q < frames; ++q) {
            double s = 0;
            for (size_t r = 0; r < rows; ++r) s += F[r][p] * F[r][q];
            double d = norm[p] * norm[q], v = d > 0 ? s / d : 0;
            S[p][q] = v;
            S[q][p] = v;
        }
        if (tick && p % 32 == 0 && !tick((double)p / frames)) return {};
    }
    return S;
}
// The novelty of a self-similarity matrix: correlated along its diagonal
// with a Gaussian-tapered checkerboard kernel of width frames (Foote 2000):
// the mean similarity within the past and within the future, minus the mean
// similarity between them, each weighted by the kernel and taken only over
// what lies inside the matrix (0 near the ends, where less than half
// of the kernel fits). High where
// what comes before is unlike what comes after; about 0..1.
inline Vec footeNovelty(const Mat& S, int width) {
    size_t frames = S.size();
    width = std::max(2, width + (width & 1));
    int h = width / 2;
    Vec g((size_t)width);
    double sigma = h / 2.0;
    for (int k = 0; k < width; ++k) { double d = k - h + .5; g[(size_t)k] = std::exp(-.5 * d * d / (sigma * sigma)); }
    double full = 0;                                   // the weight of each half of the whole kernel
    for (int u = 0; u < width; ++u)
        for (int v = 0; v < width; ++v)
            if ((u < h) == (v < h)) full += g[(size_t)u] * g[(size_t)v];
    Vec nov(frames, 0.0);
    for (size_t f = 0; f < frames; ++f) {
        double same = 0, ws = 0, cross = 0, wc = 0;
        for (int u = -h; u < h; ++u)
            for (int v = -h; v < h; ++v) {
                long p = (long)f + u, q = (long)f + v;
                if (p < 0 || q < 0 || p >= (long)frames || q >= (long)frames) continue;
                double w = g[(size_t)(u + h)] * g[(size_t)(v + h)], x = S[(size_t)p][(size_t)q];
                if ((u < 0) == (v < 0)) { same += w * x; ws += w; }
                else { cross += w * x; wc += w; }
            }
        bool enough = ws > .5 * full && wc > .5 * full;   // too little of the kernel inside: unreliable
        nov[f] = enough ? std::max(0.0, same / ws - cross / wc) : 0.0;
    }
    return nov;
}
// the frames of the peaks of a novelty curve above threshold times its
// moving median over width frames (plus 5% of its maximum), at least `gap`
// frames apart: the boundaries of the sections
inline std::vector<int> noveltyPeaks(const Vec& nov, double threshold, int width, int gap) {
    std::vector<int> out;
    if (nov.size() < 3) return out;
    Vec med = medianFilter(nov, width);
    double floor = .05 * maxOf(nov);
    int last = -1000000;
    for (int k : localMaxima(nov)) {
        if (nov[(size_t)k] <= threshold * (med[(size_t)k] + floor)) continue;
        if (k - last >= gap) { out.push_back(k); last = k; }
        else if (nov[(size_t)k] > nov[(size_t)last]) { out.back() = k; last = k; }
    }
    return out;
}

// ------------------------------------------------------------------ curves
// RBJ biquad, for the loudness weighting
struct Biquad2 {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void design(bool high, double sr, double f0, double q) {
        double w0 = kTwoPi * f0 / sr, cs = std::cos(w0), alpha = std::sin(w0) / (2 * q), a0 = 1 + alpha;
        if (high) { b0 = (1 + cs) / 2; b1 = -(1 + cs); }
        else { b0 = (1 - cs) / 2; b1 = 1 - cs; }
        b2 = b0;
        b0 /= a0; b1 /= a0; b2 /= a0;
        a1 = -2 * cs / a0;
        a2 = (1 - alpha) / a0;
    }
    double run(double x) {
        double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};
// The level of every hop in dB, roughly as the ear weighs it (below 150 Hz
// and above 8 kHz taken out); a value a frame, centred like the spectra.
inline Vec loudnessCurve(const std::vector<float>& x, double sr, int hop, int n) {
    Biquad2 hp, lp;
    hp.design(true, sr, 150, .7);
    lp.design(false, sr, std::min(8000.0, .45 * sr), .7);
    std::vector<double> w(x.size());
    for (size_t k = 0; k < x.size(); ++k) w[k] = lp.run(hp.run(x[k]));
    int frames = frameCount((long)x.size(), hop);
    Vec out((size_t)frames);
    for (int f = 0; f < frames; ++f) {
        long a = std::max(0L, (long)f * hop - n / 2), b = std::min((long)x.size(), (long)f * hop + n / 2);
        double s = 0;
        for (long k = a; k < b; ++k) s += w[(size_t)k] * w[(size_t)k];
        out[(size_t)f] = b > a ? toDb(2 * s / (double)(b - a)) : kFloorDb;   // a full-scale sine: 0 dB
    }
    return out;
}
// Where the sound sits, every frame: the centroid in MIDI pitch (powers
// weighting the pitch of every bin above 30 Hz, so an octave is an octave),
// the spread around it in octaves, and the lowest strong partial in MIDI
// pitch (roughly where the register begins). Silent frames keep the values
// of the frame before.
inline Mat registerCurve(const Mat& mag, double sr, int n) {
    size_t frames = mag.size();
    Mat out(3, Vec(frames, 0.0));
    if (!frames) return out;
    size_t bins = mag[0].size();
    Vec pitch(bins, 0.0);
    std::vector<char> keep(bins, 0);
    for (size_t b = 0; b < bins; ++b) {
        double f = b * sr / n;
        keep[b] = f > 30;
        pitch[b] = 69 + 12 * std::log2(std::max(f, 30.0) / 440);
    }
    double c = 60, s = 1, low = 48;
    for (size_t f = 0; f < frames; ++f) {
        const Vec& m = mag[f];
        double mx = 0, sw = 0, sp = 0;
        for (size_t b = 0; b < bins; ++b) mx = std::max(mx, m[b]);
        if (mx > 1e-3) {                                         // above -60 dB
            for (size_t b = 0; b < bins; ++b)
                if (keep[b]) { double w = m[b] * m[b]; sw += w; sp += w * pitch[b]; }
            if (sw > 0) {
                c = sp / sw;
                double v = 0;
                for (size_t b = 0; b < bins; ++b)
                    if (keep[b]) v += m[b] * m[b] * (pitch[b] - c) * (pitch[b] - c);
                s = std::sqrt(v / sw) / 12;
            }
            for (size_t b = 1; b + 1 < bins; ++b)
                if (keep[b] && m[b] > m[b - 1] && m[b] >= m[b + 1] && m[b] > .15 * mx) { low = pitch[b]; break; }
        }
        out[0][f] = c;
        out[1][f] = s;
        out[2][f] = low;
    }
    return out;
}
// How many voices seem to sound in each frame: the spectral peaks above 15%
// of the frame's strongest, about three strong partials a voice (1..16;
// 0 for a silent frame). A rough estimate, not a transcription.
inline Vec polyphonyCurve(const Mat& mag) {
    Vec out(mag.size(), 0.0);
    for (size_t f = 0; f < mag.size(); ++f) {
        const Vec& m = mag[f];
        double mx = maxOf(m);
        if (mx <= 1e-3) continue;
        int peaks = 0;
        for (size_t b = 1; b + 1 < m.size(); ++b)
            if (m[b] > m[b - 1] && m[b] >= m[b + 1] && m[b] > .15 * mx) ++peaks;
        out[f] = std::clamp(std::round(peaks / 3.0), 1.0, 16.0);
    }
    return out;
}

}  // namespace mir
}  // namespace musil
