// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// maple.h — matching pursuit, after the author's Maple: a sound explained as
// a sum of atoms taken from a dictionary, the strongest first.
//
// Two kinds of dictionary:
//   gabor, gammatone   windowed cosines at every scale (2^j samples), time
//                      position and frequency: Gaussian windows, or the
//                      fast attack and slow decay of a gammatone envelope.
//                      The pursuit is an STFT per scale with local updates
//                      (FFT matching, as MPTK): every time position on a
//                      grid of a quarter of the scale, every frequency (the
//                      peak refined by a parabola), the optimal phase by an
//                      exact projection on the cosine and sine atoms.
//   sounds             atoms cut from recordings (frames, onsets or whole
//                      files), each of unit energy. The target is cut into
//                      segments (onsets, or blocks) and each is explained by
//                      a few atoms; every candidate atom slides over the
//                      segment by FFT cross-correlation (Maple placed atoms
//                      only at the start), and a quick comparison of mel
//                      spectra keeps the best candidates for that test.
// A decomposition (a "book") is a list of atoms: where, how long, which
// frequency or which dictionary atom, how strong, with which phase.
#pragma once

#include "signals/mir.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <cmath>
#include <complex>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace musil {
namespace maple {

using Tick = std::function<bool(double)>;   // progress 0..1; false stops

// ------------------------------------------------------------------ dictionary
struct SoundAtom {
    std::vector<float> x;          // unit energy
    std::vector<double> prefix;    // energy of the first k samples (k = 0..n)
    std::vector<float> mel;        // mean log-mel spectrum, mean removed, unit norm
    std::string source;            // file name
    double start = 0;              // where it was cut (s)
    double centroid = 0;           // spectral centroid (Hz)
    float peak = 0;                // its largest sample
};

struct Dictionary {
    std::string kind = "empty";    // gabor, gammatone, sounds
    double sr = 44100;
    int minScale = 256, maxScale = 8192;   // parametric: window lengths (powers of 2)
    double fmin = 40, fmax = 16000;
    std::vector<SoundAtom> atoms;
    bool parametric() const { return kind == "gabor" || kind == "gammatone"; }
    size_t size() const {
        if (!parametric()) return atoms.size();
        size_t n = 0;
        for (int s = minScale; s <= maxScale; s *= 2) ++n;
        return n;
    }
};

// the window of a parametric atom of length n (peak 1)
inline std::vector<float> window(bool gamma, int n) {
    std::vector<float> w((size_t)n);
    for (int t = 0; t < n; ++t) {
        double u = (t + .5) / n;
        if (gamma) {                          // t^3 e^(-12 t): a gammatone's envelope
            double v = std::pow(u, 3) * std::exp(-12 * u), peak = std::pow(.25, 3) * std::exp(-3.0);
            w[(size_t)t] = (float)(v / peak);
        } else {
            w[(size_t)t] = (float)std::exp(-18 * (u - .5) * (u - .5));   // Gaussian, +-3 sigma
        }
    }
    return w;
}

// descriptor of a stretch of sound for the preselection: its mean log-mel
// spectrum, the mean removed, unit norm
class MelDescriptor {
public:
    MelDescriptor(double sr) {
        spec_.init(kN);
        bank_.init(sr, kN, kBands);
    }
    std::vector<float> operator()(const float* x, long n) {
        std::vector<double> acc(kBands, 0.0), mel;
        std::vector<float> buf(kN);
        mir::Vec mag;
        int frames = 0;
        for (long s = 0; s < std::max(1L, n - kN / 2); s += kN / 2) {
            for (int k = 0; k < kN; ++k) buf[(size_t)k] = s + k < n ? x[s + k] : 0.f;
            spec_.magnitudes(buf.data(), mag);
            bank_.apply(mag, mel);
            for (int b = 0; b < kBands; ++b) acc[(size_t)b] += mel[(size_t)b];
            ++frames;
        }
        double m = 0, e = 0;
        for (auto& v : acc) { v /= std::max(1, frames); m += v; }
        m /= kBands;
        for (auto& v : acc) { v -= m; e += v * v; }
        e = std::sqrt(e);
        std::vector<float> out(kBands);
        for (int b = 0; b < kBands; ++b) out[(size_t)b] = e > 0 ? (float)(acc[(size_t)b] / e) : 0.f;
        return out;
    }

private:
    static constexpr int kN = 2048, kBands = 40;
    mir::FrameSpectrum spec_;
    mir::MelBank bank_;
};

// the spectral centroid of a sound (Hz)
inline double centroidOf(const std::vector<float>& x, double sr) {
    const int n = 4096;
    mir::FrameSpectrum fs;
    fs.init(n);
    std::vector<float> buf(n);
    mir::Vec mag;
    double sw = 0, sf = 0;
    for (size_t s = 0; s < std::max<size_t>(1, x.size()); s += n) {
        for (int k = 0; k < n; ++k) buf[(size_t)k] = s + (size_t)k < x.size() ? x[s + (size_t)k] : 0.f;
        fs.magnitudes(buf.data(), mag);
        for (size_t b = 0; b < mag.size(); ++b) { sw += mag[b]; sf += mag[b] * b * sr / n; }
    }
    return sw > 0 ? sf / sw : 0;
}

// a sound atom from samples: normalised, described
inline bool makeSoundAtom(std::vector<float> x, double sr, const std::string& source, double start,
                          MelDescriptor& desc, SoundAtom& out) {
    double e = 0;
    for (float v : x) e += (double)v * v;
    if (x.size() < 64 || e < 1e-10) return false;
    float g = (float)(1.0 / std::sqrt(e));
    for (auto& v : x) v *= g;
    out.prefix.assign(x.size() + 1, 0.0);
    float peak = 0;
    for (size_t k = 0; k < x.size(); ++k) {
        out.prefix[k + 1] = out.prefix[k] + (double)x[k] * x[k];
        peak = std::max(peak, std::fabs(x[k]));
    }
    out.mel = desc(x.data(), (long)x.size());
    out.centroid = centroidOf(x, sr);
    out.peak = peak;
    out.source = source;
    out.start = start;
    out.x = std::move(x);
    return true;
}

// ------------------------------------------------------------------ book
struct BookAtom {
    double start = 0;      // samples (may be negative: the atom begins before the sound)
    int length = 0;        // samples
    double freq = 0;       // Hz (gabor), or the atom's centroid (sounds)
    double amp = 0;        // gabor: amplitude of the cosine; sounds: coefficient of the unit atom
    double phase = 0;      // gabor
    int index = -1;        // sounds: which atom
    double energy = 0;     // the energy it explains
};
struct Book {
    std::vector<BookAtom> atoms;
    double sr = 44100;
    long length = 0;       // of the sound, in samples
    double energy = 0, residual = 0;
};

// The largest of a vector of energies and where it is (the first place, on a tie), kept up to date as single
// values change: a tournament tree, O(log n) an update instead of a scan of every frame at every atom
class MaxTree {
public:
    void build(const std::vector<float>* v) {
        v_ = v; n_ = (int)v->size(); size_ = 1;
        while (size_ < std::max(1, n_)) size_ <<= 1;
        t_.assign((size_t)(2 * size_), -1);
        for (int i = 0; i < n_; ++i) t_[(size_t)(size_ + i)] = i;
        for (int i = size_ - 1; i >= 1; --i) t_[(size_t)i] = better(t_[(size_t)(2 * i)], t_[(size_t)(2 * i + 1)]);
    }
    void update(int i) {
        for (int k = (size_ + i) / 2; k >= 1; k /= 2) t_[(size_t)k] = better(t_[(size_t)(2 * k)], t_[(size_t)(2 * k + 1)]);
    }
    int top() const { return size_ > 0 && !t_.empty() ? t_[1 < t_.size() ? 1 : 0] : -1; }

private:
    int better(int a, int b) const {
        if (a < 0) return b;
        if (b < 0) return a;
        float va = (*v_)[(size_t)a], vb = (*v_)[(size_t)b];
        return vb > va ? b : a;                      // a is left of b: it wins a tie
    }
    const std::vector<float>* v_ = nullptr;
    int n_ = 0, size_ = 0;
    std::vector<int> t_;
};

// ------------------------------------------------------------------ parametric pursuit
class ParametricPursuit {
public:
    ParametricPursuit(const Dictionary& d, std::vector<float> x, double sr) : gamma_(d.kind == "gammatone"), sr_(sr) {
        len_ = (long)x.size();
        for (int s = d.minScale; s <= d.maxScale; s *= 2) {
            Scale sc;
            sc.n = s;
            sc.hop = std::max(1, s / 4);
            sc.M = 2 * s;
            sc.w = window(gamma_, s);
            sc.W = 0;
            for (float v : sc.w) sc.W += (double)v * v;
            std::vector<float> re((size_t)sc.M, 0.f), im((size_t)sc.M, 0.f);
            for (int t = 0; t < s; ++t) re[(size_t)t] = sc.w[(size_t)t] * sc.w[(size_t)t];
            sc.fft.init(sc.M);
            sc.fft.forward(re.data(), im.data());
            sc.g2re = re;
            sc.g2im = im;
            sc.kmin = std::max(1, (int)std::ceil(std::max(d.fmin, sr / s) * sc.M / sr));
            sc.kmax = std::min(sc.M / 2 - 2, (int)std::floor(std::min(d.fmax, .49 * sr) * sc.M / sr));
            scales_.push_back(std::move(sc));
        }
        pad_ = d.maxScale;
        r_.assign((size_t)(len_ + 2 * pad_), 0.f);
        for (long i = 0; i < len_; ++i) r_[(size_t)(i + pad_)] = x[(size_t)i];
        for (auto& sc : scales_) {
            long frames = ((long)r_.size() - sc.n) / sc.hop + 1;
            sc.best.assign((size_t)std::max(0L, frames), 0.f);
            sc.bin.assign(sc.best.size(), 0);
            sc.re.assign((size_t)sc.M, 0.f);
            sc.im.assign((size_t)sc.M, 0.f);
        }
    }
    double energy() const {
        double e = 0;
        for (float v : r_) e += (double)v * v;
        return e;
    }
    // runs until maxAtoms are found or the residual is snrDb under the sound
    bool run(Book& book, int maxAtoms, double snrDb, const Tick& tick) {
        book.sr = sr_;
        book.length = len_;
        book.energy = energy();
        double target = book.energy * std::pow(10.0, -snrDb / 10.0), e = book.energy;
        for (size_t s = 0; s < scales_.size(); ++s) {
            for (long f = 0; f < (long)scales_[s].best.size(); ++f) update(s, f, false);
            scales_[s].tree.build(&scales_[s].best);
            if (tick && !tick(.1 * (s + 1) / scales_.size())) return false;
        }
        for (int a = 0; a < maxAtoms && e > target; ++a) {
            // the strongest frame of all: the first scale whose best is strictly larger (as a scan in order would)
            size_t bs = 0;
            long bf = -1;
            float bE = 0;
            for (size_t s = 0; s < scales_.size(); ++s) {
                int f = scales_[s].tree.top();
                if (f >= 0 && scales_[s].best[(size_t)f] > bE) { bE = scales_[s].best[(size_t)f]; bs = s; bf = f; }
            }
            if (bf < 0 || bE < 1e-14) break;
            BookAtom at = select(bs, bf);
            if (at.energy <= 0) { scales_[bs].best[(size_t)bf] = 0; scales_[bs].tree.update((int)bf); continue; }
            e = std::max(0.0, e - at.energy);
            book.atoms.push_back(at);
            // the frames that overlap the atom change
            long a0 = (long)(at.start + pad_), a1 = a0 + at.length;
            for (size_t s = 0; s < scales_.size(); ++s) {
                const Scale& sc = scales_[s];
                long f0 = std::max(0L, (a0 - sc.n) / sc.hop), f1 = std::min((long)sc.best.size() - 1, a1 / sc.hop);
                for (long f = f0; f <= f1; ++f) update(s, f);
            }
            if (tick && a % 16 == 0) {
                double p = std::max((double)a / maxAtoms, 1.0 - std::log(std::max(e, target) / target) /
                                                                std::max(1e-9, std::log(book.energy / target)));
                if (!tick(.1 + .9 * std::clamp(p, 0.0, 1.0))) return false;
            }
        }
        book.residual = energy();
        return true;
    }
    // the residual, without the padding
    std::vector<float> residual() const {
        return std::vector<float>(r_.begin() + pad_, r_.begin() + pad_ + len_);
    }

private:
    struct Scale {
        int n = 0, hop = 0, M = 0, kmin = 1, kmax = 1;
        std::vector<float> w, g2re, g2im, re, im, best;
        std::vector<int> bin;
        double W = 0;
        mir::FFT fft;
        MaxTree tree;                                // over best
    };
    // the projection on cos and sin at bin k of a frame's spectrum: energy, coefficients
    static double project(const Scale& sc, double a, double b, double c2re, double c2im, double& alpha, double& beta) {
        double C = .5 * (sc.W + c2re), S = .5 * (sc.W - c2re), X = -.5 * c2im;
        double det = C * S - X * X;
        if (det <= 1e-12 * sc.W * sc.W) return 0;
        alpha = (S * a - X * b) / det;
        beta = (C * b - X * a) / det;
        return alpha * a + beta * b;
    }
    void update(size_t s, long f, bool track = true) {
        Scale& sc = scales_[s];
        long p = f * sc.hop;
        std::fill(sc.re.begin(), sc.re.end(), 0.f);
        std::fill(sc.im.begin(), sc.im.end(), 0.f);
        for (int t = 0; t < sc.n; ++t) sc.re[(size_t)t] = r_[(size_t)(p + t)] * sc.w[(size_t)t];
        sc.fft.forward(sc.re.data(), sc.im.data());
        float best = 0;
        int bk = 0;
        for (int k = sc.kmin; k <= sc.kmax; ++k) {
            double a = sc.re[(size_t)k], b = -sc.im[(size_t)k];          // sum r w cos, sum r w sin
            int k2 = (2 * k) % sc.M;
            double al, be, E = project(sc, a, b, sc.g2re[(size_t)k2], sc.g2im[(size_t)k2], al, be);
            if (E > best) { best = (float)E; bk = k; }
        }
        sc.best[(size_t)f] = best;
        sc.bin[(size_t)f] = bk;
        if (track) sc.tree.update((int)f);
    }
    // the exact projection at angular frequency w (rad/sample) on the frame at p
    double exact(const Scale& sc, long p, double w, double& alpha, double& beta) const {
        double a = 0, b = 0, c2 = 0, s2 = 0;
        for (int t = 0; t < sc.n; ++t) {
            double ww = sc.w[(size_t)t], x = r_[(size_t)(p + t)] * ww;
            a += x * std::cos(w * t);
            b += x * std::sin(w * t);
            c2 += ww * ww * std::cos(2 * w * t);
            s2 += ww * ww * std::sin(2 * w * t);
        }
        // project() takes the spectrum of w^2 at 2w: re = sum w^2 cos, im = -sum w^2 sin
        return project(sc, a, b, c2, -s2, alpha, beta);
    }
    BookAtom select(size_t s, long f) {
        Scale& sc = scales_[s];
        long p = f * sc.hop;
        int k = sc.bin[(size_t)f];
        // refine the frequency: a parabola through the energies at k-1, k, k+1
        auto energyAt = [&](double kk) {
            double al, be;
            return exact(sc, p, mir::kTwoPi * kk / sc.M, al, be);
        };
        double e0 = energyAt(k - 1), e1 = energyAt(k), e2 = energyAt(k + 1), d = e0 - 2 * e1 + e2;
        double kk = k + (d < 0 ? std::clamp(.5 * (e0 - e2) / d, -.5, .5) : 0.0);
        double w = mir::kTwoPi * kk / sc.M, al = 0, be = 0;
        double E = exact(sc, p, w, al, be);
        if (E < e1) { w = mir::kTwoPi * k / sc.M; E = exact(sc, p, w, al, be); }
        BookAtom at;
        if (E <= 0) return at;
        for (int t = 0; t < sc.n; ++t)
            r_[(size_t)(p + t)] -= (float)(sc.w[(size_t)t] * (al * std::cos(w * t) + be * std::sin(w * t)));
        at.start = (double)(p - pad_);
        at.length = sc.n;
        at.freq = w * sr_ / mir::kTwoPi;
        at.amp = std::sqrt(al * al + be * be);
        at.phase = std::atan2(be, al);               // al cos + be sin = amp cos(w t - phase)
        at.energy = E;
        return at;
    }
    std::vector<Scale> scales_;
    std::vector<float> r_;
    bool gamma_;
    double sr_;
    long len_ = 0, pad_ = 0;
};

// ------------------------------------------------------------------ pursuit with sound atoms
struct SoundPursuitOptions {
    int components = 4;          // atoms a segment
    int candidates = 16;         // atoms tried by cross-correlation, after the mel preselection
    bool onsets = true;          // segments at the onsets, or blocks
    double block = .1;           // s, the blocks' length (or the longest segment)
    double threshold = 2;        // onsets
};

inline bool soundPursuit(const Dictionary& d, std::vector<float> x, double sr, const SoundPursuitOptions& o,
                         Book& book, std::vector<float>& residual, const Tick& tick) {
    book.sr = sr;
    book.length = (long)x.size();
    book.energy = 0;
    for (float v : x) book.energy += (double)v * v;
    if (d.atoms.empty()) return false;
    // the segments
    std::vector<long> cuts{0};
    long maxSeg = std::max(256L, (long)(o.block * sr));
    if (o.onsets) {
        mir::Mat mag = mir::magSpectrogram(x, 1024, 256);
        mir::Vec flux = mir::fluxOf(mag);
        for (double t : mir::onsetTimes(x, sr, 1024, 256, flux, o.threshold, 21, .05)) cuts.push_back((long)(t * sr));
    }
    cuts.push_back((long)x.size());
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    std::vector<std::pair<long, long>> segs;           // long ones are cut into blocks
    for (size_t k = 0; k + 1 < cuts.size(); ++k)
        for (long a = cuts[k]; a < cuts[k + 1]; a += maxSeg) {
            long b = std::min(cuts[k + 1], a + maxSeg);
            if (b - a >= 32) segs.push_back({a, b});
        }
    MelDescriptor desc(sr);
    residual = x;
    const long total = (long)x.size();
    // the FFTs are cached: one plan per size; each atom's spectrum per size, for the whole pursuit (up to a budget
    // of memory); the residual's per size and length, for the candidates of one component. The products are the
    // same floats as before, so the book is the same
    std::map<long, mir::FFT> plans;
    auto plan = [&](long P) -> mir::FFT& { auto& f = plans[P]; if (f.size() != (int)P) f.init((int)P); return f; };
    struct Spec { std::vector<float> re, im; };
    std::map<std::pair<int, long>, Spec> atomSpec;
    size_t atomFloats = 0; const size_t atomBudget = (size_t)1 << 25;      // 32M floats: 128 MB
    std::vector<float> rr, ri;
    for (size_t sg = 0; sg < segs.size(); ++sg) {
        long a = segs[sg].first, S = segs[sg].second - a;
        for (int c = 0; c < o.components; ++c) {
            double segE = 0;
            for (long t = 0; t < S; ++t) segE += (double)residual[(size_t)(a + t)] * residual[(size_t)(a + t)];
            if (segE < 1e-10) break;
            // preselection: the atoms whose mel spectrum is closest
            std::vector<float> dm = desc(&residual[(size_t)a], S);
            std::vector<std::pair<float, int>> sim;
            for (size_t k = 0; k < d.atoms.size(); ++k) {
                float s = 0;
                for (size_t b = 0; b < dm.size(); ++b) s += dm[b] * d.atoms[k].mel[b];
                sim.push_back({s, (int)k});
            }
            int keep = std::min((int)sim.size(), std::max(1, o.candidates));
            std::partial_sort(sim.begin(), sim.begin() + keep, sim.end(),
                              [](auto& p, auto& q) { return p.first > q.first; });
            double bestG = 0, bestCoef = 0;
            int bestK = -1;
            long bestTau = 0;
            std::map<std::pair<long, long>, Spec> resSpec;            // (P, W): the residual's spectrum
            for (int ci = 0; ci < keep; ++ci) {
                const SoundAtom& at = d.atoms[(size_t)sim[(size_t)ci].second];
                // the atom starts in the segment but is matched (and later
                // subtracted) whole, over what follows it too
                long n = (long)at.x.size(), W = std::min(S + n, total - a), P = 1;
                while (P < S + n) P <<= 1;
                mir::FFT& fft = plan(P);
                auto rk = std::make_pair(P, W);
                auto rit = resSpec.find(rk);
                if (rit == resSpec.end()) {
                    Spec sp; sp.re.assign((size_t)P, 0.f); sp.im.assign((size_t)P, 0.f);
                    for (long t = 0; t < W; ++t) sp.re[(size_t)t] = residual[(size_t)(a + t)];
                    fft.forward(sp.re.data(), sp.im.data());
                    rit = resSpec.emplace(rk, std::move(sp)).first;
                }
                auto ak = std::make_pair(sim[(size_t)ci].second, P);
                auto ait = atomSpec.find(ak);
                const Spec* asp = ait != atomSpec.end() ? &ait->second : nullptr;
                Spec fresh;
                if (!asp) {
                    fresh.re.assign((size_t)P, 0.f); fresh.im.assign((size_t)P, 0.f);
                    for (long t = 0; t < n; ++t) fresh.re[(size_t)t] = at.x[(size_t)t];
                    fft.forward(fresh.re.data(), fresh.im.data());
                    if (atomFloats + 2 * (size_t)P <= atomBudget) { atomFloats += 2 * (size_t)P; asp = &atomSpec.emplace(ak, std::move(fresh)).first->second; }
                    else asp = &fresh;
                }
                rr.resize((size_t)P); ri.resize((size_t)P);
                const Spec& R = rit->second;
                for (long k = 0; k < P; ++k) {             // R conj(A)
                    float xr = R.re[(size_t)k], xi = R.im[(size_t)k], yr = asp->re[(size_t)k], yi = -asp->im[(size_t)k];
                    rr[(size_t)k] = xr * yr - xi * yi;
                    ri[(size_t)k] = xr * yi + xi * yr;
                }
                fft.inverse(rr.data(), ri.data());
                for (long tau = 0; tau < S; ++tau) {
                    double cor = rr[(size_t)tau] / P;
                    long m = std::min(n, W - tau);
                    double pe = at.prefix[(size_t)m];
                    if (pe < .05) continue;                  // too little of the atom fits
                    double g = cor * cor / pe;
                    if (g > bestG) { bestG = g; bestCoef = cor / pe; bestK = sim[(size_t)ci].second; bestTau = tau; }
                }
            }
            if (bestK < 0 || bestG < 1e-12) break;
            const SoundAtom& at = d.atoms[(size_t)bestK];
            long m = std::min((long)at.x.size(), total - (a + bestTau));
            for (long t = 0; t < m; ++t) residual[(size_t)(a + bestTau + t)] -= (float)(bestCoef * at.x[(size_t)t]);
            BookAtom b;
            b.start = (double)(a + bestTau);
            b.length = (int)std::min(m, S - bestTau);      // the part inside its segment (tails 0 plays only that)
            b.freq = at.centroid;
            b.amp = bestCoef;
            b.index = bestK;
            b.energy = bestG;
            book.atoms.push_back(b);
        }
        if (tick && sg % 4 == 0 && !tick((double)(sg + 1) / segs.size())) return false;
    }
    book.residual = 0;
    for (float v : residual) book.residual += (double)v * v;
    return true;
}

// ------------------------------------------------------------------ synthesis
struct SynthOptions {
    double ratio = 1;      // transposition
    double stretch = 1;    // time
    bool tails = true;     // sound atoms play whole (they ring on), or only the part that matched
    double keep = 1;       // the share of the atoms used, strongest first (0..1)
};

inline std::vector<float> synthesize(const Book& book, const Dictionary& d, const SynthOptions& o) {
    std::vector<size_t> order(book.atoms.size());
    for (size_t k = 0; k < order.size(); ++k) order[k] = k;
    std::sort(order.begin(), order.end(), [&](size_t p, size_t q) { return book.atoms[p].energy > book.atoms[q].energy; });
    size_t use = (size_t)std::ceil(std::clamp(o.keep, 0.0, 1.0) * (double)order.size());
    long len = (long)(book.length * o.stretch) + 1;
    long extra = 0;
    for (size_t k = 0; k < use; ++k) {
        const BookAtom& b = book.atoms[order[k]];
        long n = b.index >= 0 && b.index < (int)d.atoms.size() && o.tails ? (long)d.atoms[(size_t)b.index].x.size() : b.length;
        extra = std::max(extra, (long)(n / std::max(.05, o.ratio)));
    }
    std::vector<float> out((size_t)(len + extra), 0.f);
    bool gamma = d.kind == "gammatone";
    std::map<int, std::vector<float>> windows;
    for (size_t k = 0; k < use; ++k) {
        const BookAtom& b = book.atoms[order[k]];
        long at = (long)std::lround(b.start * o.stretch);
        if (b.index < 0) {                                       // a parametric atom
            if (!windows.count(b.length)) windows[b.length] = window(gamma, b.length);
            const auto& w = windows[b.length];
            double om = mir::kTwoPi * b.freq * o.ratio / book.sr;
            if (b.freq * o.ratio >= book.sr / 2) continue;
            for (int t = 0; t < b.length; ++t) {
                long i = at + t;
                if (i >= 0 && i < (long)out.size()) out[(size_t)i] += (float)(b.amp * w[(size_t)t] * std::cos(om * t - b.phase));
            }
        } else if (b.index < (int)d.atoms.size()) {             // a sound atom, resampled by ratio
            const auto& x = d.atoms[(size_t)b.index].x;
            long n = o.tails ? (long)x.size() : std::min((long)x.size(), (long)std::max(1, b.length));
            long m = (long)(n / std::max(.05, o.ratio));
            for (long t = 0; t < m; ++t) {
                double pos = t * o.ratio;
                long i0 = (long)pos;
                if (i0 + 1 >= (long)x.size()) break;
                double fr = pos - i0;
                long i = at + t;
                if (i >= 0 && i < (long)out.size())
                    out[(size_t)i] += (float)(b.amp * (x[(size_t)i0] + fr * (x[(size_t)i0 + 1] - x[(size_t)i0])));
            }
        }
    }
    // trim the silent end
    long last = (long)out.size() - 1;
    while (last > len && std::fabs(out[(size_t)last]) < 1e-6f) --last;
    out.resize((size_t)std::max(1L, last + 1));
    return out;
}

// ------------------------------------------------------------------ building dictionaries of sounds
// x resampled from one rate to another (linear: dictionaries and sounds meet at one rate)
inline std::vector<float> resampleTo(const std::vector<float>& x, double from, double to) {
    if (from == to || from <= 0 || to <= 0) return x;
    double r = from / to;
    std::vector<float> y((size_t)(x.size() / r));
    for (size_t i = 0; i < y.size(); ++i) {
        double p = i * r;
        size_t a = (size_t)p;
        float f = (float)(p - a);
        y[i] = a + 1 < x.size() ? x[a] + f * (x[a + 1] - x[a]) : (a < x.size() ? x[a] : 0.f);
    }
    return y;
}

// a fade in and out of n samples (at most half the sound each)
inline void fade(std::vector<float>& x, long n) {
    n = std::min(n, (long)x.size() / 2);
    for (long i = 0; i < n; ++i) {
        float g = (float)i / n;
        x[(size_t)i] *= g;
        x[x.size() - 1 - (size_t)i] *= g;
    }
}

// How a sound becomes atoms: Hann-windowed frames (ms long, every hop ms), the spans between its onsets
// (threshold; maxMs at most), or whole (maxMs at most)
struct CutOptions {
    std::string mode = "frames";    // frames, onsets, whole
    double ms = 100, hop = 100, threshold = 2, maxMs = 2000;
};

inline void cutSound(const std::vector<float>& x, double sr, const CutOptions& c, const std::string& source,
                     MelDescriptor& desc, std::vector<SoundAtom>& out) {
    auto add = [&](std::vector<float> v, double start) {
        SoundAtom a;
        if (makeSoundAtom(std::move(v), sr, source, start, desc, a)) out.push_back(std::move(a));
    };
    long maxN = (long)(c.maxMs / 1000.0 * sr);
    if (c.mode == "frames") {
        long n = std::max(64L, (long)(c.ms / 1000.0 * sr)), hop = std::max(16L, (long)(c.hop / 1000.0 * sr));
        for (long s = 0; s + n <= (long)x.size(); s += hop) {
            std::vector<float> v(x.begin() + s, x.begin() + s + n);
            for (long t = 0; t < n; ++t) v[(size_t)t] *= (float)(.5 - .5 * std::cos(mir::kTwoPi * t / n));
            add(std::move(v), s / sr);
        }
    } else if (c.mode == "onsets") {
        mir::Mat mag = mir::magSpectrogram(x, 1024, 256);
        mir::Vec flux = mir::fluxOf(mag);
        std::vector<long> cuts{0};
        for (double t : mir::onsetTimes(x, sr, 1024, 256, flux, c.threshold, 21, .05)) cuts.push_back((long)(t * sr));
        cuts.push_back((long)x.size());
        std::sort(cuts.begin(), cuts.end());
        for (size_t k = 0; k + 1 < cuts.size(); ++k) {
            long a = cuts[k], b = std::min(cuts[k + 1], a + maxN);
            if (b - a < (long)(.02 * sr)) continue;
            std::vector<float> v(x.begin() + a, x.begin() + b);
            fade(v, (long)(.01 * sr));
            add(std::move(v), a / sr);
        }
    } else {                                                   // whole
        std::vector<float> v(x.begin(), x.begin() + std::min((long)x.size(), maxN));
        fade(v, (long)(.01 * sr));
        add(std::move(v), 0);
    }
}

// a dictionary of Gabor or gammatone atoms at a rate: window lengths from lo to hi samples (powers of 2,
// 32 to 65536), frequencies up to fmax (at least 100 Hz)
inline Dictionary parametric(bool gamma, double sr, int lo, int hi, double fmax) {
    auto pow2 = [](int v) { int n = 1; while (n < v) n <<= 1; return n; };
    Dictionary d;
    d.kind = gamma ? "gammatone" : "gabor";
    d.sr = sr;
    lo = pow2(lo);
    hi = pow2(hi);
    d.minScale = std::clamp(std::min(lo, hi), 32, 65536);
    d.maxScale = std::clamp(std::max(lo, hi), 32, 65536);
    d.fmax = std::max(100.0, fmax);
    return d;
}

// ------------------------------------------------------------------ decomposition, cross synthesis
// x explained by the atoms of d: a parametric dictionary at the sound's rate (maxAtoms, or until the
// residual is snrDb under the sound), a dictionary of sounds at its own rate (x is resampled to it; the
// options say how many atoms a segment). False when stopped.
inline bool decompose(const Dictionary& d, std::vector<float> x, double sr, int maxAtoms, double snrDb,
                      const SoundPursuitOptions& so, Book& book, std::vector<float>& residual, const Tick& tick) {
    if (!d.parametric() && d.sr != sr) {
        x = resampleTo(x, sr, d.sr);
        sr = d.sr;
    }
    if (d.parametric()) {
        ParametricPursuit pp(d, std::move(x), sr);
        if (!pp.run(book, maxAtoms, snrDb, tick)) return false;
        residual = pp.residual();
        return true;
    }
    return soundPursuit(d, std::move(x), sr, so, book, residual, tick);
}

// a book of the sound atoms of d made a book of the atoms of to: each atom replaced by the closest of the
// other dictionary (their mel spectra), so the sound is rebuilt from the other's material
inline Book crossBook(const Book& book, const Dictionary& d, const Dictionary& to) {
    Book out = book;
    std::vector<int> map(d.atoms.size(), 0);
    for (size_t i = 0; i < d.atoms.size(); ++i) {
        float best = -2;
        for (size_t j = 0; j < to.atoms.size(); ++j) {
            float s = 0;
            for (size_t k = 0; k < d.atoms[i].mel.size() && k < to.atoms[j].mel.size(); ++k) s += d.atoms[i].mel[k] * to.atoms[j].mel[k];
            if (s > best) { best = s; map[i] = (int)j; }
        }
    }
    for (auto& a : out.atoms)
        if (a.index >= 0 && a.index < (int)map.size()) a.index = map[(size_t)a.index];
    return out;
}

// the signal-to-residual ratio of a book (dB)
inline double snrOf(const Book& b) { return b.residual > 0 ? 10 * std::log10(b.energy / b.residual) : 120.0; }

// ------------------------------------------------------------------ a book as rows
// A book as 8 rows, one value per atom: 0 time (s, the atom's centre), 1 frequency (Hz), 2 level (dB),
// 3 dictionary atom (-1: parametric), 4 length (samples), 5 amplitude, 6 phase, 7 start (samples). Plotted
// as a scatter (time, frequency, level) it is an atom map.
inline std::vector<std::vector<double>> bookRows(const Book& book, const Dictionary& d) {
    std::vector<std::vector<double>> ch(8);
    for (auto& a : book.atoms) {
        double lev = a.index >= 0 && a.index < (int)d.atoms.size() ? std::fabs(a.amp) * d.atoms[(size_t)a.index].peak : a.amp;
        ch[0].push_back((a.start + a.length / 2.0) / book.sr);
        ch[1].push_back(a.freq);
        ch[2].push_back(20 * std::log10(std::max(lev, 1e-9)));
        ch[3].push_back(a.index);
        ch[4].push_back(a.length);
        ch[5].push_back(a.amp);
        ch[6].push_back(a.phase);
        ch[7].push_back(a.start);
    }
    return ch;
}
// a book back from its rows (at a rate): what synthesize needs
inline Book bookFromRows(const std::vector<std::vector<double>>& ch, double sr) {
    Book book;
    book.sr = sr;
    if (ch.size() < 8) return book;
    for (size_t f = 0; f < ch[0].size(); ++f) {
        auto at = [&](int c) { return f < ch[(size_t)c].size() ? ch[(size_t)c][f] : 0.0; };
        BookAtom a;
        a.freq = at(1);
        a.index = (int)at(3);
        a.length = (int)at(4);
        a.amp = at(5);
        a.phase = at(6);
        a.start = at(7);
        a.energy = a.amp * a.amp * (a.index < 0 ? a.length * .3 : 1.0);   // for the ordering by strength
        book.length = std::max(book.length, (long)(a.start + a.length));
        book.atoms.push_back(a);
    }
    return book;
}

// ------------------------------------------------------------------ dictionaries in files
// A dictionary in a file: its kind and parameters, then every atom (source, start, samples). Written as
// MAPLEATOMS1; files written by Moritz before (MORITZATOMS1) read the same.
inline bool saveDictionary(const Dictionary& d, const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto w32 = [&](int32_t v) { f.write((const char*)&v, 4); };
    auto wd = [&](double v) { f.write((const char*)&v, 8); };
    auto ws = [&](const std::string& s) { w32((int32_t)s.size()); f.write(s.data(), (std::streamsize)s.size()); };
    f.write("MAPLEATOMS1 ", 12);
    ws(d.kind);
    wd(d.sr); w32(d.minScale); w32(d.maxScale); wd(d.fmin); wd(d.fmax);
    w32((int32_t)d.atoms.size());
    for (auto& a : d.atoms) {
        ws(a.source);
        wd(a.start);
        w32((int32_t)a.x.size());
        f.write((const char*)a.x.data(), (std::streamsize)(a.x.size() * sizeof(float)));
    }
    return (bool)f;
}
inline bool loadDictionary(const std::string& path, Dictionary& out, const Tick& tick = {}) {
    std::ifstream f(path, std::ios::binary);
    char magic[12];
    if (!f.read(magic, 12)) return false;
    std::string m(magic, 12);
    if (m != "MAPLEATOMS1 " && m != "MORITZATOMS1") return false;
    auto r32 = [&]() { int32_t v = 0; f.read((char*)&v, 4); return v; };
    auto rd = [&]() { double v = 0; f.read((char*)&v, 8); return v; };
    auto rs = [&]() { std::string s((size_t)std::max(0, r32()), ' '); f.read(&s[0], (std::streamsize)s.size()); return s; };
    out = Dictionary();
    out.kind = rs();
    out.sr = rd(); out.minScale = r32(); out.maxScale = r32(); out.fmin = rd(); out.fmax = rd();
    int n = r32();
    MelDescriptor desc(out.sr);
    for (int i = 0; i < n && f; ++i) {
        std::string src = rs();
        double start = rd();
        std::vector<float> x((size_t)std::max(0, r32()));
        f.read((char*)x.data(), (std::streamsize)(x.size() * sizeof(float)));
        SoundAtom a;
        if (makeSoundAtom(std::move(x), out.sr, src, start, desc, a)) out.atoms.push_back(std::move(a));
        if (tick && i % 32 == 0 && !tick((double)i / std::max(1, n))) return false;
    }
    return true;
}

}  // namespace maple
}  // namespace musil
