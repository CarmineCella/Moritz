// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// soundtypes.h — sound types, after the author's SoundTypes (ICMC 2013): a
// sound becomes a sequence of symbols, and new sequences become new sounds.
//
// Level 1: the frames of a sound (MFCCs, centroid and loudness, or the
// atoms of a pursuit book) are clustered into k types (k-means++); runs of
// one type are the units of level 1.
// Higher levels: where the next type is hard to predict (its surprise,
// -log P(type | previous type), stands out), a gesture ends; the gestures
// (units of level 1 between two boundaries) are clustered into the types of
// level 2, and so on. This is the hierarchy SoundTypes announced; the
// original computed n-grams of level 1 only.
// Generation, at any level: the classic n-grams of SoundTypes, a
// variable-order Markov model (the longest context seen often enough), or a
// factor oracle (Allauzen, Crochemore and Raffinot 1999; navigated as in
// OMax and the Variable Markov Oracle: continue the original, or jump where
// the recent past repeats). The units chosen are realised with their own
// sound, joined by short crossfades (or rebuilt from their atoms).
// Matching: each unit of a sound replaced by the closest type of another,
// at any level: SoundTypes' rematch, now also between gestures.
#pragma once

#include "music/maple.h"
#include "signals/mir.h"
#include "../rng.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace musil {
namespace soundtypes {

using Vec = std::vector<double>;

struct Unit {
    long start = 0, end = 0;     // samples
    int type = 0;
    int first = 0, last = 0;     // the units of the level below it spans (inclusive), or its frames at level 1
    Vec feat;                    // what it sounds like (comparable between sounds: means, spreads, duration)
};
struct Level {
    int k = 0;                   // number of types
    std::vector<Unit> units;
    std::vector<Vec> centroid;   // per type, in the comparable features
};

struct Model {
    double sr = 44100;
    int hop = 512, size = 2048;
    std::vector<float> sound;                     // the sound (mono), to realise units
    std::shared_ptr<const maple::Book> book;      // or its atoms
    std::shared_ptr<const maple::Dictionary> dict;
    std::vector<Vec> frames;                      // frame features (raw)
    std::vector<int> frameType;                   // level-1 type of every frame
    std::vector<Level> levels;
    std::vector<long> onsets;                     // if any: level-1 units are the spans between them
    long length() const { return book ? book->length : (long)sound.size(); }
};

struct Options {
    std::vector<int> k{12, 6, 4};   // types per level
    int levels = 2;
    double threshold = 1.0;         // boundaries: surprise above mean + threshold * sd
    int smooth = 5;                 // level-1 labels: majority over this many frames (odd)
    unsigned seed = 1;
};

// ------------------------------------------------------------------ helpers
inline double dist2(const Vec& a, const Vec& b) {
    double s = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) s += (a[i] - b[i]) * (a[i] - b[i]);
    return s;
}
// standardises the columns of rows in place; returns mean and sd
inline void standardize(std::vector<Vec>& rows, Vec* meanOut = nullptr, Vec* sdOut = nullptr) {
    if (rows.empty()) return;
    size_t d = rows[0].size();
    Vec m(d, 0.0), s(d, 0.0);
    for (auto& r : rows) for (size_t i = 0; i < d; ++i) m[i] += r[i];
    for (auto& v : m) v /= (double)rows.size();
    for (auto& r : rows) for (size_t i = 0; i < d; ++i) s[i] += (r[i] - m[i]) * (r[i] - m[i]);
    for (auto& v : s) v = std::sqrt(v / (double)rows.size());
    for (auto& r : rows) for (size_t i = 0; i < d; ++i) r[i] = s[i] > 1e-12 ? (r[i] - m[i]) / s[i] : 0;
    if (meanOut) *meanOut = m;
    if (sdOut) *sdOut = s;
}
// k-means++ (the best of a few starts); labels and centroids
inline void kmeans(const std::vector<Vec>& x, int k, unsigned seed, std::vector<int>& label, std::vector<Vec>& cent) {
    size_t n = x.size();
    k = std::max(1, std::min(k, (int)n));
    std::mt19937 rng(seed);
    double bestCost = 1e300;
    std::vector<int> bestL;
    std::vector<Vec> bestC;
    for (int start = 0; start < 4; ++start) {
        std::vector<Vec> c;
        c.push_back(x[rnd::below(rng, (uint32_t)n)]);
        Vec dmin(n, 1e300);
        while ((int)c.size() < k) {
            double tot = 0;
            for (size_t i = 0; i < n; ++i) { dmin[i] = std::min(dmin[i], dist2(x[i], c.back())); tot += dmin[i]; }
            double r = rnd::uniform(rng, 0, tot);
            size_t pick = 0;
            for (double acc = 0; pick < n; ++pick) { acc += dmin[pick]; if (acc >= r) break; }
            c.push_back(x[std::min(pick, n - 1)]);
        }
        std::vector<int> l(n, 0);
        double cost = 0;
        for (int it = 0; it < 50; ++it) {
            bool changed = false;
            cost = 0;
            for (size_t i = 0; i < n; ++i) {
                int b = 0;
                double bd = 1e300;
                for (int j = 0; j < k; ++j) { double dd = dist2(x[i], c[(size_t)j]); if (dd < bd) { bd = dd; b = j; } }
                if (l[i] != b) { l[i] = b; changed = true; }
                cost += bd;
            }
            std::vector<Vec> s((size_t)k, Vec(x[0].size(), 0.0));
            std::vector<int> cnt((size_t)k, 0);
            for (size_t i = 0; i < n; ++i) { ++cnt[(size_t)l[i]]; for (size_t d = 0; d < x[i].size(); ++d) s[(size_t)l[i]][d] += x[i][d]; }
            for (int j = 0; j < k; ++j)
                if (cnt[(size_t)j]) for (auto& v : s[(size_t)j]) v /= cnt[(size_t)j];
                else s[(size_t)j] = x[rnd::below(rng, (uint32_t)n)];
            c = s;
            if (!changed && it > 0) break;
        }
        if (cost < bestCost) { bestCost = cost; bestL = l; bestC = c; }
    }
    // renumber the types by first appearance, so type 0 starts the sound
    std::vector<int> map((size_t)k, -1);
    int next = 0;
    for (int v : bestL) if (map[(size_t)v] < 0) map[(size_t)v] = next++;
    for (auto& v : map) if (v < 0) v = next++;
    label.resize(n);
    for (size_t i = 0; i < n; ++i) label[i] = map[(size_t)bestL[i]];
    cent.assign((size_t)k, Vec());
    for (int j = 0; j < k; ++j) cent[(size_t)map[(size_t)j]] = bestC[(size_t)j];
}

// ------------------------------------------------------------------ analysis
// frame features of a sound: 13 MFCCs, log centroid, level (dB); with
// pitch, also the 12 pitch classes (chroma, weighted by 3 so that harmony
// counts as much as timbre)
inline std::vector<Vec> soundFeatures(const std::vector<float>& x, double sr, int n, int hop, const maple::Tick& tick,
                                      bool pitch = false) {
    mir::Mat mag = mir::magSpectrogram(x, n, hop, tick);
    mir::MelBank bank;
    bank.init(sr, n, 40);
    mir::ChromaMap cmap = mir::chromaMap(sr, n);
    std::vector<Vec> out;
    mir::Vec mel, c, chr;
    for (auto& m : mag) {
        bank.apply(m, mel);
        mir::dct(mel, 13, c);
        double sw = 0, sf = 0, e = 0;
        for (size_t b = 0; b < m.size(); ++b) { sw += m[b]; sf += m[b] * b * sr / n; e += m[b] * m[b]; }
        Vec f(c.begin(), c.end());
        f.push_back(std::log2(std::max(sw > 0 ? sf / sw : 100.0, 50.0)));
        f.push_back(mir::toDb(e * 2.0 / n));
        if (pitch) {
            mir::chroma(m, cmap, chr);
            for (double v : chr) f.push_back(3 * v);
        }
        out.push_back(std::move(f));
    }
    return out;
}
// frame features of a book: the atoms' energy in 40 log-frequency bands
// (50 Hz to 16 kHz), log-compressed; an atom counts in the frames it covers
inline std::vector<Vec> bookFeatures(const maple::Book& b, int hop) {
    long frames = b.length / hop + 1;
    std::vector<Vec> out((size_t)frames, Vec(40, 0.0));
    for (auto& a : b.atoms) {
        double f = std::clamp(a.freq, 50.0, 16000.0);
        int band = std::clamp((int)(40 * std::log(f / 50.0) / std::log(16000.0 / 50.0)), 0, 39);
        long f0 = std::max(0L, (long)a.start / hop), f1 = std::min(frames - 1, (long)(a.start + a.length) / hop);
        for (long fr = f0; fr <= f1; ++fr) out[(size_t)fr][(size_t)band] += a.energy / (f1 - f0 + 1);
    }
    for (auto& v : out) for (auto& e : v) e = std::log1p(1e3 * e);
    return out;
}

// the comparable description of a stretch of frames: mean and spread of the
// raw features, and its log duration
inline Vec describe(const std::vector<Vec>& frames, int a, int b, double seconds) {
    size_t d = frames.empty() ? 0 : frames[0].size();
    Vec m(d, 0.0), s(d, 0.0);
    int n = std::max(1, b - a + 1);
    for (int f = a; f <= b; ++f) for (size_t i = 0; i < d; ++i) m[i] += frames[(size_t)f][i];
    for (auto& v : m) v /= n;
    for (int f = a; f <= b; ++f) for (size_t i = 0; i < d; ++i) s[i] += (frames[(size_t)f][i] - m[i]) * (frames[(size_t)f][i] - m[i]);
    Vec out = m;
    for (auto& v : s) out.push_back(std::sqrt(v / n));
    out.push_back(std::log(std::max(seconds, 1e-3)));
    return out;
}

// boundaries of a symbol sequence where the next symbol is surprising:
// -log P(s[i] | s[i-1]) (bigram, add-0.5) above mean + z * sd; gestures of
// at least 2 and at most 24 units
inline std::vector<int> surpriseCuts(const std::vector<int>& s, int k, double z) {
    std::vector<int> cuts{0};
    if (s.size() < 3) { cuts.push_back((int)s.size()); return cuts; }
    std::vector<Vec> count((size_t)k, Vec((size_t)k, .5));
    for (size_t i = 1; i < s.size(); ++i) count[(size_t)s[i - 1]][(size_t)s[i]] += 1;
    Vec sur(s.size(), 0.0);
    double mean = 0, var = 0;
    for (size_t i = 1; i < s.size(); ++i) {
        double row = 0;
        for (double v : count[(size_t)s[i - 1]]) row += v;
        sur[i] = -std::log(count[(size_t)s[i - 1]][(size_t)s[i]] / row);
        mean += sur[i];
    }
    mean /= (double)(s.size() - 1);
    for (size_t i = 1; i < s.size(); ++i) var += (sur[i] - mean) * (sur[i] - mean);
    double sd = std::sqrt(var / (double)(s.size() - 1)), level = mean + z * sd;
    int last = 0;
    for (size_t i = 1; i < s.size(); ++i) {
        bool peak = sur[i] >= sur[i - 1] && (i + 1 >= s.size() || sur[i] >= sur[i + 1]);
        if (((sur[i] > level && peak) || (int)i - last >= 24) && (int)i - last >= 2) { cuts.push_back((int)i); last = (int)i; }
    }
    if ((int)s.size() - last < 2 && cuts.size() > 1) cuts.pop_back();
    cuts.push_back((int)s.size());
    return cuts;
}

// the onsets of a sound (samples), at least gap seconds apart
inline std::vector<long> soundOnsets(const std::vector<float>& x, double sr, double threshold, double gap) {
    mir::Mat mag = mir::magSpectrogram(x, 1024, 256);
    mir::Vec flux = mir::fluxOf(mag);
    std::vector<long> out;
    for (double t : mir::onsetTimes(x, sr, 1024, 256, flux, threshold, 21, gap)) out.push_back((long)(t * sr));
    return out;
}

// level 1 from onsets: the spans between them (notes, strokes), described
// and clustered as wholes; a first span shorter than 50 ms joins the next
inline bool onsetLevel(Model& m, const Options& o, Level& l1) {
    std::vector<long> cuts{0};
    for (long t : m.onsets) if (t > cuts.back() + (long)(.05 * m.sr) && t < m.length() - (long)(.05 * m.sr)) cuts.push_back(t);
    cuts.push_back(m.length());
    if (cuts.size() < 3) return false;
    long frames = (long)m.frames.size();
    std::vector<Vec> desc;
    for (size_t c = 0; c + 1 < cuts.size(); ++c) {
        Unit u;
        u.start = cuts[c];
        u.end = cuts[c + 1];
        u.first = (int)std::min(frames - 1, u.start / m.hop);
        u.last = (int)std::clamp(u.end / m.hop - 1, (long)u.first, frames - 1);
        u.feat = describe(m.frames, u.first, u.last, (u.end - u.start) / m.sr);
        desc.push_back(u.feat);
        l1.units.push_back(u);
    }
    standardize(desc);
    std::vector<int> lab;
    std::vector<Vec> cent;
    kmeans(desc, std::min(o.k.empty() ? 12 : o.k[0], (int)l1.units.size()), o.seed, lab, cent);
    l1.k = (int)cent.size();
    m.frameType.assign((size_t)frames, 0);
    for (size_t i = 0; i < l1.units.size(); ++i) {
        l1.units[i].type = lab[i];
        for (long f = l1.units[i].start / m.hop; f < std::min(frames, (l1.units[i].end + m.hop - 1) / m.hop); ++f) m.frameType[(size_t)f] = lab[i];
    }
    return true;
}

inline bool analyse(Model& m, const Options& o, const maple::Tick& tick) {
    if (m.frames.empty()) return false;
    Level l1;
    if (!m.onsets.empty() && onsetLevel(m, o, l1)) {
        // level 1 made of notes: nothing else to do here
    } else {
    // level 1: frames into types, then runs
    std::vector<Vec> z = m.frames;
    standardize(z);
    std::vector<int> lab;
    kmeans(z, o.k.empty() ? 12 : o.k[0], o.seed, lab, l1.centroid);
    l1.k = (int)l1.centroid.size();
    if (o.smooth > 1) {                                       // majority over a few frames: fewer one-frame runs
        std::vector<int> sm(lab.size());
        int h = o.smooth / 2;
        for (size_t i = 0; i < lab.size(); ++i) {
            std::map<int, int> votes;
            for (long j = (long)i - h; j <= (long)i + h; ++j)
                if (j >= 0 && j < (long)lab.size()) ++votes[lab[(size_t)j]];
            int best = lab[i], bc = 0;
            for (auto& kv : votes) if (kv.second > bc || (kv.second == bc && kv.first == lab[i])) { best = kv.first; bc = kv.second; }
            sm[i] = best;
        }
        lab = sm;
    }
    m.frameType = lab;
    for (size_t i = 0; i < lab.size();) {
        size_t j = i;
        while (j + 1 < lab.size() && lab[j + 1] == lab[i]) ++j;
        Unit u;
        u.type = lab[i];
        u.first = (int)i;
        u.last = (int)j;
        u.start = (long)i * m.hop;
        u.end = std::min(m.length(), (long)(j + 1) * m.hop);
        u.feat = describe(m.frames, u.first, u.last, (u.end - u.start) / m.sr);
        l1.units.push_back(u);
        i = j + 1;
    }
    }
    // the comparable centroids: mean description of each type's units
    auto centroids = [](Level& L) {
        L.centroid.assign((size_t)L.k, Vec());
        std::vector<int> cnt((size_t)L.k, 0);
        for (auto& u : L.units) {
            Vec& c = L.centroid[(size_t)u.type];
            if (c.empty()) c.assign(u.feat.size(), 0.0);
            for (size_t i = 0; i < u.feat.size(); ++i) c[i] += u.feat[i];
            ++cnt[(size_t)u.type];
        }
        for (int t = 0; t < L.k; ++t) if (cnt[(size_t)t]) for (auto& v : L.centroid[(size_t)t]) v /= cnt[(size_t)t];
    };
    centroids(l1);
    m.levels.assign(1, l1);
    if (tick && !tick(.8)) return false;
    // higher levels: gestures between surprising transitions, clustered
    for (int L = 1; L < o.levels; ++L) {
        const Level& below = m.levels[(size_t)L - 1];
        if (below.units.size() < 4) break;
        std::vector<int> sym;
        for (auto& u : below.units) sym.push_back(u.type);
        std::vector<int> cuts = surpriseCuts(sym, below.k, o.threshold);
        Level up;
        std::vector<Vec> desc;
        for (size_t c = 0; c + 1 < cuts.size(); ++c) {
            Unit u;
            u.first = cuts[c];
            u.last = cuts[c + 1] - 1;
            u.start = below.units[(size_t)u.first].start;
            u.end = below.units[(size_t)u.last].end;
            int f0 = (int)(u.start / m.hop), f1 = std::max(f0, (int)(u.end / m.hop) - 1);
            f1 = std::min(f1, (int)m.frames.size() - 1);
            u.feat = describe(m.frames, f0, f1, (u.end - u.start) / m.sr);
            // for clustering also what it is made of: its types below, by duration
            Vec d = u.feat, hist((size_t)below.k, 0.0);
            double tot = 0;
            for (int i = u.first; i <= u.last; ++i) {
                double dur = (double)(below.units[(size_t)i].end - below.units[(size_t)i].start);
                hist[(size_t)below.units[(size_t)i].type] += dur;
                tot += dur;
            }
            for (double h : hist) d.push_back(tot > 0 ? h / tot : 0);
            desc.push_back(d);
            up.units.push_back(u);
        }
        if (up.units.size() < 2) break;
        standardize(desc);
        std::vector<int> l;
        std::vector<Vec> cent;
        int k = L < (int)o.k.size() ? o.k[(size_t)L] : std::max(2, o.k.back() / 2);
        kmeans(desc, std::min(k, (int)up.units.size()), o.seed + (unsigned)L, l, cent);
        up.k = (int)cent.size();
        for (size_t i = 0; i < up.units.size(); ++i) up.units[i].type = l[i];
        centroids(up);
        m.levels.push_back(up);
        if (tick && !tick(.8 + .2 * L / std::max(1, o.levels - 1))) return false;
    }
    return true;
}

// the type of every frame at each level: a matrix for plot (a row a level)
inline std::vector<std::vector<float>> typeStrips(const Model& m) {
    size_t frames = m.frames.size();
    std::vector<std::vector<float>> rows;
    for (auto& L : m.levels) {
        std::vector<float> r(frames, 0.f);
        for (auto& u : L.units)
            for (long f = u.start / m.hop; f < std::min((long)frames, (u.end + m.hop - 1) / m.hop); ++f) r[(size_t)f] = (float)u.type;
        rows.push_back(std::move(r));
    }
    return rows;
}

// transition probabilities between the types of a level (rows: from)
inline std::vector<std::vector<float>> transitions(const Level& L) {
    std::vector<std::vector<float>> t((size_t)L.k, std::vector<float>((size_t)L.k, 0.f));
    for (size_t i = 1; i < L.units.size(); ++i) t[(size_t)L.units[i - 1].type][(size_t)L.units[i].type] += 1;
    for (auto& r : t) {
        float s = 0;
        for (float v : r) s += v;
        if (s > 0) for (auto& v : r) v /= s;
    }
    return t;
}

// ------------------------------------------------------------------ generation
enum class Method { Classic, Markov, Oracle };
struct GenOptions {
    Method method = Method::Oracle;
    int order = 2;            // classic: n (the context is n-1 symbols); markov: the longest context
    int minCount = 2;         // markov: how often a context must appear to be trusted
    double continuity = .8;   // oracle: chance of following the original
    int context = 1;          // oracle: the shortest repeated past a jump needs
    unsigned seed = 1;
};

// a factor oracle on a symbol sequence: suffix links and their repeated lengths
struct Oracle {
    std::vector<std::map<int, int>> trans;   // state -> symbol -> state
    std::vector<int> sfx, lrs;
    // the length of the common suffix of the prefixes ending at states p1 and p2
    int commonSuffix(int p1, int p2) const {
        if (p2 < 0 || p1 < 0) return 0;
        if (p2 == sfx[(size_t)p1]) return lrs[(size_t)p1];
        while (p2 > 0 && sfx[(size_t)p2] != sfx[(size_t)p1]) p2 = sfx[(size_t)p2];
        return std::min(lrs[(size_t)p1], lrs[(size_t)std::max(0, p2)]);
    }
    explicit Oracle(const std::vector<int>& s) {
        size_t n = s.size();
        trans.assign(n + 1, {});
        sfx.assign(n + 1, -1);
        lrs.assign(n + 1, 0);
        for (size_t i = 1; i <= n; ++i) {
            int sym = s[i - 1];
            trans[i - 1][sym] = (int)i;
            int k = sfx[i - 1], pi = (int)i - 1;
            while (k > -1 && !trans[(size_t)k].count(sym)) {
                trans[(size_t)k][sym] = (int)i;
                pi = k;
                k = sfx[(size_t)k];
            }
            if (k == -1) { sfx[i] = 0; lrs[i] = 0; }
            else {
                sfx[i] = trans[(size_t)k][sym];
                lrs[i] = commonSuffix(pi, sfx[i] - 1) + 1;     // Lefebvre and Lecroq
            }
        }
    }
};

// One step of an oracle walk from state (after unit state-1): the next
// state, following the original with probability continuity, otherwise
// jumping after a place where the last `context` symbols repeat.
inline int oracleStep(const Oracle& orc, int state, double continuity, int context, std::mt19937& rng) {
    int n = (int)orc.sfx.size() - 1;
    if (state < n && (rnd::uniform01(rng) < continuity || state == 0)) return state + 1;
    // jump: to the suffix link, or to a state linking back to it, if the shared past is long enough
    std::vector<int> cand;
    int s = orc.sfx[(size_t)state];
    if (s >= 0 && orc.lrs[(size_t)state] >= context) cand.push_back(s);
    for (int j = 1; j <= n; ++j)
        if (orc.sfx[(size_t)j] == s && j != state && orc.lrs[(size_t)j] >= context) cand.push_back(j);
    int from = cand.empty() ? (int)rnd::below(rng, (uint32_t)n)
                            : cand[rnd::below(rng, (uint32_t)cand.size())];
    int next = std::min(from + 1, n);
    if (next == state) next = (int)rnd::between(rng, 1, n);
    return next;
}

// Generates a sequence of unit indices of level L (the original's units)
// until `seconds` of sound; the units are chosen by their types, or (the
// oracle) by their place in the original.
inline std::vector<int> generate(const Model& m, int level, double seconds, const GenOptions& o) {
    const Level& L = m.levels[(size_t)std::clamp(level, 0, (int)m.levels.size() - 1)];
    std::vector<int> out;
    size_t n = L.units.size();
    if (!n) return out;
    std::mt19937 rng(o.seed);
    std::vector<int> sym;
    for (auto& u : L.units) sym.push_back(u.type);
    std::vector<std::vector<int>> instances((size_t)L.k);
    for (size_t i = 0; i < n; ++i) instances[(size_t)sym[i]].push_back((int)i);
    long need = (long)(seconds * m.sr), have = 0;
    auto push = [&](int u) {
        out.push_back(u);
        have += L.units[(size_t)u].end - L.units[(size_t)u].start;
    };
    // a unit for a type: preferably the one that follows the last in the original
    auto realise = [&](int type) {
        if (!out.empty() && out.back() + 1 < (int)n && sym[(size_t)out.back() + 1] == type) return out.back() + 1;
        const auto& in = instances[(size_t)type];
        return in[rnd::below(rng, (uint32_t)in.size())];
    };
    if (o.method == Method::Oracle) {
        Oracle orc(sym);
        int state = 0;                                  // the oracle's state i stands after unit i-1
        while (have < need && out.size() < 100000) {
            int next = oracleStep(orc, state, o.continuity, o.context, rng);
            push(next - 1);
            state = next >= (int)n ? 0 : next;          // at the end: start again
        }
        return out;
    }
    // counts of contexts: up to `order` symbols before the next
    int D = std::max(1, o.method == Method::Classic ? o.order - 1 : o.order);
    std::map<std::vector<int>, std::map<int, int>> counts;
    for (size_t i = 0; i < n; ++i)
        for (int d = 0; d <= D && d <= (int)i; ++d) {
            std::vector<int> ctx(sym.begin() + (long)i - d, sym.begin() + (long)i);
            ++counts[ctx][sym[i]];
        }
    std::vector<int> hist;
    while (have < need && out.size() < 100000) {
        const std::map<int, int>* dist = nullptr;
        if (o.method == Method::Classic) {
            if ((int)hist.size() >= D) {
                std::vector<int> ctx(hist.end() - D, hist.end());
                auto it = counts.find(ctx);
                if (it != counts.end()) dist = &it->second;
            }
            if (!dist && (int)hist.size() >= D) {           // a dead end: start again from a random place
                hist.clear();
            }
            if (!dist) {
                int u = (int)rnd::below(rng, (uint32_t)n);
                for (int d = 0; d < D && u + d < (int)n; ++d) { hist.push_back(sym[(size_t)(u + d)]); push(u + d); }
                continue;
            }
        } else {                                             // the longest context seen often enough
            for (int d = std::min(D, (int)hist.size()); d >= 0; --d) {
                std::vector<int> ctx(hist.end() - d, hist.end());
                auto it = counts.find(ctx);
                if (it == counts.end()) continue;
                int tot = 0;
                for (auto& kv : it->second) tot += kv.second;
                if (tot >= o.minCount || d == 0) { dist = &it->second; break; }
            }
        }
        int tot = 0;
        for (auto& kv : *dist) tot += kv.second;
        int r = (int)rnd::below(rng, (uint32_t)tot), type = dist->begin()->first;
        for (auto& kv : *dist) { if (r < kv.second) { type = kv.first; break; } r -= kv.second; }
        hist.push_back(type);
        if ((int)hist.size() > D + 1) hist.erase(hist.begin());
        push(realise(type));
    }
    return out;
}

// Levels mixed: a sequence of units of level L played with the level below
// regenerated. Every unit keeps its first sub-unit and its number of
// sub-units, but the walk through them is an oracle walk of the level below
// (continuity c): the form comes from level L, the detail is new. Repeated
// down to level 1; returns units of level 1, and with c = 1 the same sound.
inline std::vector<int> nest(const Model& m, int level, const std::vector<int>& seq, double continuity, int context, unsigned seed) {
    std::vector<int> cur = seq;
    std::mt19937 rng(seed * 7919u + 13u);
    for (int L = level; L > 0; --L) {
        const Level& up = m.levels[(size_t)L];
        const Level& below = m.levels[(size_t)L - 1];
        std::vector<int> sym;
        for (auto& u : below.units) sym.push_back(u.type);
        Oracle orc(sym);
        int n = (int)below.units.size();
        std::vector<int> down;
        for (int u : cur) {
            const Unit& U = up.units[(size_t)u];
            int count = U.last - U.first + 1, state = U.first + 1;
            down.push_back(U.first);
            for (int k = 1; k < count; ++k) {
                int next = oracleStep(orc, state >= n ? 0 : state, continuity, context, rng);
                down.push_back(next - 1);
                state = next;
            }
        }
        cur.swap(down);
    }
    return cur;
}

// ------------------------------------------------------------------ realisation
// The sound of a span of the original [a, b): its samples (or its atoms).
inline std::vector<float> span(const Model& m, long a, long b) {
    if (m.book && m.dict) {
        maple::Book part;
        part.sr = m.book->sr;
        part.length = b - a;
        for (auto& at : m.book->atoms)
            if (at.start >= a && at.start < b) { maple::BookAtom c = at; c.start -= a; part.atoms.push_back(c); }
        maple::SynthOptions so;
        std::vector<float> y = maple::synthesize(part, *m.dict, so);
        return y;
    }
    a = std::clamp(a, 0L, (long)m.sound.size());
    b = std::clamp(b, a, (long)m.sound.size());
    return std::vector<float>(m.sound.begin() + a, m.sound.begin() + b);
}

// joins pieces one after the other (or at given times), with crossfades
struct Piece {
    long src0 = 0, src1 = 0;   // span in the source
    long at = -1;              // where it goes (-1: after the previous)
    long len = -1;             // its length (-1: its own)
};
inline std::vector<float> join(const Model& m, const std::vector<Piece>& pieces, double xfadeMs) {
    long half = std::max(1L, (long)(xfadeMs / 2000.0 * m.sr)), total = m.length();
    std::vector<float> out;
    long pos = 0;
    for (auto& p : pieces) {
        long own = p.src1 - p.src0, len = p.len >= 0 ? p.len : own;
        // a little of the source on both sides (where there is some), for the crossfades
        long a = std::max(0L, p.src0 - half), pre = p.src0 - a;
        long b = std::min(total, p.src0 + len + half), post = std::max(0L, b - (p.src0 + len));
        std::vector<float> y = span(m, a, b);
        long fin = std::max(2 * pre, std::min(64L, (long)y.size() / 4)), fout = std::max(2 * post, std::min(64L, (long)y.size() / 4));
        fin = std::min(fin, (long)y.size() / 2);
        fout = std::min(fout, (long)y.size() / 2);
        for (long i = 0; i < fin; ++i) y[(size_t)i] *= (float)(.5 - .5 * std::cos(mir::kPi * (i + .5) / fin));
        for (long i = 0; i < fout; ++i) y[y.size() - 1 - (size_t)i] *= (float)(.5 - .5 * std::cos(mir::kPi * (i + .5) / fout));
        long at = (p.at >= 0 ? p.at : pos) - pre;
        if (at < 0) { y.erase(y.begin(), y.begin() + std::min((long)y.size(), -at)); at = 0; }
        if ((long)out.size() < at + (long)y.size()) out.resize((size_t)(at + (long)y.size()), 0.f);
        for (size_t i = 0; i < y.size(); ++i) out[(size_t)at + i] += y[i];
        pos = (p.at >= 0 ? p.at : pos) + len;
    }
    return out;
}

// the level-1 type of every frame of a generated sound (for plot)
inline std::vector<std::vector<float>> generatedStrips(const Model& m, int level, const std::vector<int>& seq) {
    std::vector<std::vector<float>> rows(m.levels.size());
    const Level& L = m.levels[(size_t)level];
    std::vector<std::vector<float>> src = typeStrips(m);
    for (int u : seq) {
        long f0 = L.units[(size_t)u].start / m.hop, f1 = (L.units[(size_t)u].end + m.hop - 1) / m.hop;
        for (size_t r = 0; r < rows.size(); ++r)
            for (long f = f0; f < f1 && f < (long)src[r].size(); ++f) rows[r].push_back(src[r][(size_t)f]);
    }
    return rows;
}

// The amplitude envelope of a target imposed on a sound by an amount c (0:
// none, 1: all): gain (env target / env sound)^c, envelopes the RMS over
// 20 ms, the gain at most +20 dB (no noise floor pulled up); the overall
// level stays the sound's.
inline void contour(std::vector<float>& y, const std::vector<float>& target, double sr, double c) {
    if (c <= 0 || y.empty() || target.empty()) return;
    long w = std::max(16L, (long)(.02 * sr)), hop = w / 2;
    auto env = [&](const std::vector<float>& x, long frames) {
        Vec e((size_t)frames, 0.0);
        for (long f = 0; f < frames; ++f) {
            double s = 0;
            long a = f * hop - w / 2, n = 0;
            for (long t = std::max(0L, a); t < std::min((long)x.size(), a + w); ++t, ++n) s += (double)x[(size_t)t] * x[(size_t)t];
            e[(size_t)f] = n ? std::sqrt(s / w) : 0.0;
        }
        return e;
    };
    long frames = (long)y.size() / hop + 2;
    Vec ey = env(y, frames), et = env(target, frames);
    double top = 0;
    for (double v : ey) top = std::max(top, v);
    Vec g((size_t)frames, 1.0);
    for (long f = 0; f < frames; ++f)
        g[(size_t)f] = std::min(10.0, std::pow(et[(size_t)f] / std::max(ey[(size_t)f], 1e-3 * top + 1e-9), c));
    double before = 0, after = 0;
    for (size_t t = 0; t < y.size(); ++t) {
        double p = (double)t / hop;
        size_t f = (size_t)p;
        double fr = p - (double)f, gg = f + 1 < g.size() ? g[f] + fr * (g[f + 1] - g[f]) : g.back();
        before += (double)y[t] * y[t];
        y[t] = (float)(y[t] * gg);
        after += (double)y[t] * y[t];
    }
    if (after > 0)                                       // the shape of A's loudness, at the level of the sound
        for (auto& v : y) v = (float)(v * std::sqrt(before / after));
}

// ------------------------------------------------------------------ matching
// For each unit of A at a level, the unit of B of the closest type (the
// type's centroid against A's unit, features standardised over both), the
// instance of closest duration; at A's times (fit: cut or extended to A's
// duration) or one after the other at their own lengths.
inline std::vector<Piece> match(const Model& A, const Model& B, int level, bool fit, bool own = true,
                                std::vector<int>* typesOut = nullptr) {
    const Level& la = A.levels[(size_t)std::clamp(level, 0, (int)A.levels.size() - 1)];
    const Level& lb = B.levels[(size_t)std::clamp(level, 0, (int)B.levels.size() - 1)];
    if (la.units.empty() || lb.units.empty() || la.units[0].feat.size() != lb.units[0].feat.size()) return {};
    // the units of A, the centroids of B's types, the units of B; standardised
    // in each sound's own space (a sound's brightest maps to the other's
    // brightest), or in one space for both (absolute distances)
    std::vector<Vec> ua, ub;
    for (auto& u : la.units) ua.push_back(u.feat);
    for (auto& u : lb.units) ub.push_back(u.feat);
    std::vector<Vec> all;
    if (own) {
        standardize(ua);
        standardize(ub);
        all = ua;
    } else {
        std::vector<Vec> both = ua;
        both.insert(both.end(), ub.begin(), ub.end());
        standardize(both);
        all.assign(both.begin(), both.begin() + (long)ua.size());
        ub.assign(both.begin() + (long)ua.size(), both.end());
    }
    std::vector<Vec> cb((size_t)lb.k);
    std::vector<int> nb((size_t)lb.k, 0);
    for (size_t j = 0; j < lb.units.size(); ++j) {
        Vec& c = cb[(size_t)lb.units[j].type];
        if (c.empty()) c.assign(ub[j].size(), 0.0);
        for (size_t d = 0; d < c.size(); ++d) c[d] += ub[j][d];
        ++nb[(size_t)lb.units[j].type];
    }
    for (int t = 0; t < lb.k; ++t) if (nb[(size_t)t]) for (auto& v : cb[(size_t)t]) v /= nb[(size_t)t];
    all.insert(all.end(), cb.begin(), cb.end());
    all.insert(all.end(), ub.begin(), ub.end());
    const size_t unitsB = la.units.size() + (size_t)lb.k;
    std::vector<Piece> out;
    for (size_t i = 0; i < la.units.size(); ++i) {
        int best = 0;
        double bd = 1e300;
        for (int t = 0; t < lb.k; ++t) {
            if (all[la.units.size() + (size_t)t].empty()) continue;
            double d = dist2(all[i], all[la.units.size() + (size_t)t]);
            if (d < bd) { bd = d; best = t; }
        }
        const Unit& ua = la.units[i];
        long want = ua.end - ua.start;
        int pick = -1;                                   // the instance of that type closest to A's unit (duration included)
        double pd = std::numeric_limits<double>::max();
        for (size_t j = 0; j < lb.units.size(); ++j)
            if (lb.units[j].type == best) {
                double d = dist2(all[i], all[unitsB + j]);
                if (d < pd) { pd = d; pick = (int)j; }
            }
        if (pick < 0) continue;
        if (typesOut) typesOut->push_back(best);
        Piece p;
        p.src0 = lb.units[(size_t)pick].start;
        p.src1 = lb.units[(size_t)pick].end;
        if (fit) {
            p.at = ua.start;
            p.len = want;
            if (p.src1 - p.src0 < want) p.src1 = std::min(B.length(), p.src0 + want);   // run on into what follows
        }
        out.push_back(p);
    }
    return out;
}

// Level 0, the rematch of the original SoundTypes: every frame of A (a hop
// of A's analysis) replaced by a frame of B: the closest type of B, then its
// closest frame, each sound's features standardised on its own (or in one
// space). The frame of B is taken as a Hann grain of B's analysis size,
// centred on it, overlap-added at A's frame, with the gain (eA / eB)^c (the
// two frames' levels); the sum is divided by the overlapping windows.
inline std::vector<float> matchFrames(const Model& A, const Model& B, double c, bool own, std::vector<int>* typesOut = nullptr) {
    if (A.frames.empty() || B.frames.empty() || A.frames[0].size() != B.frames[0].size()) return {};
    std::vector<Vec> za = A.frames, zb = B.frames;
    if (own) { standardize(za); standardize(zb); }
    else {
        std::vector<Vec> both = za;
        both.insert(both.end(), zb.begin(), zb.end());
        standardize(both);
        za.assign(both.begin(), both.begin() + (long)za.size());
        zb.assign(both.begin() + (long)za.size(), both.end());
    }
    int k = B.levels.empty() ? 1 : B.levels[0].k;
    std::vector<Vec> cent((size_t)k);
    std::vector<std::vector<int>> members((size_t)k);
    for (size_t j = 0; j < zb.size(); ++j) {
        int t = j < B.frameType.size() ? std::clamp(B.frameType[j], 0, k - 1) : 0;
        members[(size_t)t].push_back((int)j);
        Vec& ct = cent[(size_t)t];
        if (ct.empty()) ct.assign(zb[j].size(), 0.0);
        for (size_t d = 0; d < ct.size(); ++d) ct[d] += zb[j][d];
    }
    for (int t = 0; t < k; ++t) if (!members[(size_t)t].empty()) for (auto& v : cent[(size_t)t]) v /= (double)members[(size_t)t].size();
    std::vector<float> src = B.book ? span(B, 0, B.length()) : B.sound;
    long N = B.size, len = A.length() + N;
    std::vector<double> win((size_t)N);
    for (long t = 0; t < N; ++t) win[(size_t)t] = .5 - .5 * std::cos(mir::kTwoPi * (t + .5) / N);
    std::vector<double> out((size_t)len, 0.0), wsum((size_t)len, 0.0);
    size_t dbIndex = A.frames[0].size() >= 15 ? 14 : A.frames[0].size() - 1;     // the level (dB) of a sound's frame
    for (size_t i = 0; i < za.size(); ++i) {
        int best = 0;
        double bd = 1e300;
        for (int t = 0; t < k; ++t) {
            if (members[(size_t)t].empty()) continue;
            double d = dist2(za[i], cent[(size_t)t]);
            if (d < bd) { bd = d; best = t; }
        }
        int pick = members[(size_t)best][0];
        double pd = 1e300;
        for (int j : members[(size_t)best]) {
            double d = dist2(za[i], zb[(size_t)j]);
            if (d < pd) { pd = d; pick = j; }
        }
        if (typesOut) typesOut->push_back(best);
        double g = 1.0;
        if (c > 0 && !A.book && !B.book) {
            double ea = std::pow(10.0, A.frames[i][dbIndex] / 20), eb = std::pow(10.0, B.frames[(size_t)pick][dbIndex] / 20);
            g = std::min(10.0, std::pow(ea / std::max(eb, 1e-5), c));
        }
        long at = (long)i * A.hop - N / 2, from = (long)pick * B.hop - N / 2;
        for (long t = 0; t < N; ++t) {
            long o = at + t, sidx = from + t;
            if (o < 0 || o >= len) continue;
            double v = sidx >= 0 && sidx < (long)src.size() ? src[(size_t)sidx] : 0.0;
            out[(size_t)o] += g * win[(size_t)t] * v;
            wsum[(size_t)o] += win[(size_t)t];
        }
    }
    std::vector<float> y((size_t)A.length());
    double peakW = 0;
    for (double w : wsum) peakW = std::max(peakW, w);
    for (size_t t = 0; t < y.size(); ++t) y[t] = (float)(out[t] / std::max(wsum[t], .25 * peakW + 1e-9));
    return y;
}

// ------------------------------------------------------------------ entry points
// How a sound is analysed: frames of size samples every hop; level 1 from runs of frames (or, with onsets,
// the spans between onsets, threshold and gap in s); pitch adds the chroma to the features
struct AnalyseOptions {
    Options types;                  // types per level, levels, threshold, smooth, seed
    int size = 2048, hop = 512;
    bool onsets = false, pitch = false;
    double onsetThreshold = 1.5, onsetGap = .08;
};

// m.sound and m.sr set (or m.book and m.dict, the atoms of a pursuit; onsets need a sound): the frames, then
// the types at each level. False when it found nothing or was stopped.
inline bool analyseModel(Model& m, const AnalyseOptions& o, const maple::Tick& tick) {
    m.size = o.size;
    m.hop = o.hop;
    auto half = [&](double p) { return tick ? tick(.5 * p) : true; };
    if (m.book) m.frames = bookFeatures(*m.book, m.hop);
    else m.frames = soundFeatures(m.sound, m.sr, m.size, m.hop, half, o.pitch);
    if (o.onsets && !m.book) m.onsets = soundOnsets(m.sound, m.sr, o.onsetThreshold, o.onsetGap);
    if (m.frames.empty() || !analyse(m, o.types, tick)) { m.levels.clear(); return false; }
    return true;
}

// a sequence of units of a level made sound, joined by crossfades (xfadeMs); with inner < 1, each gesture of
// a level above the first is played by new units of level 1 inside it (continuity inner, context
// innerContext). strips, when given, gets the types played, for a plot.
inline std::vector<float> realise(const Model& m, int level, std::vector<int> seq, double xfadeMs, double inner, int innerContext,
                                  unsigned seed, std::vector<std::vector<float>>* strips = nullptr) {
    if (m.levels.empty()) return {};
    level = std::clamp(level, 0, (int)m.levels.size() - 1);
    if (level > 0 && inner < 1) {
        seq = nest(m, level, seq, inner, innerContext, seed);
        level = 0;
    }
    std::vector<Piece> pieces;
    for (int u : seq) {
        Piece p;
        p.src0 = m.levels[(size_t)level].units[(size_t)u].start;
        p.src1 = m.levels[(size_t)level].units[(size_t)u].end;
        pieces.push_back(p);
    }
    if (strips) *strips = generatedStrips(m, level, seq);
    return join(m, pieces, xfadeMs);
}

// SoundTypes' rematch: A rebuilt with B's material, frame by frame (level 0: B's frames overlap-added at A's,
// the level contour c) or unit by unit at a level (1, 2...: at A's times and durations when fit, with A's
// loudness imposed by c; or one after the other). own: each sound's features in its own space. types gets
// the type of B used for each of A's frames or units.
inline std::vector<float> rematch(const Model& A, const Model& B, int level, bool fit, bool own, double xfadeMs, double c,
                                  std::vector<int>* types = nullptr) {
    if (A.frames.empty() || B.frames.empty() || A.frames[0].size() != B.frames[0].size()) return {};
    if (level <= 0) return matchFrames(A, B, c, own, types);
    int L = std::clamp(level, 1, (int)std::min(A.levels.size(), B.levels.size())) - 1;
    auto pieces = match(A, B, L, fit, own, types);
    std::vector<float> y = join(B, pieces, xfadeMs);
    if (fit && c > 0) {
        std::vector<float> a = A.book ? span(A, 0, A.length()) : A.sound;
        contour(y, a, B.sr, c);
    }
    return y;
}

}  // namespace soundtypes
}  // namespace musil
