// granular.h — the orchestral granulator and the morphological orchestration, without the interpreter: Musil's
// orchestrate-granular and orchestrate-morphological (music.mu wraps them: granulate, target-analyse...) and Moritz's
// orchidea in its granular and morphological modes run this code, so that for the same database, orchestra,
// parameters and seed they write the same notes.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// The granulator realises a stochastic process with the players of an orchestra: every parameter (the density, the
// register, the durations, the styles, the dynamics, the chords...) a value, a range, a set or an envelope of them
// over time (pval, env_at); at each event a free player able to play it (a sample of its instrument with the style,
// in the register, at the dynamics when strict) takes a pitch by a method (random, cluster, pivots, chordinterp,
// markov, harmonic). The morphological orchestration is the same process whose envelopes are read off a target sound
// (analyse, envelopes) and whose decision at every event is a matching pursuit over the free players' sounds at the
// target's peaks against the target's spectrum less what sounds (target_notes), or Orchidea's search there
// (target_notes_genetic). Its random draws are those the interpreted versions made, in the same order, from Musil's
// portable generator (rng.h): a seed gives the same notes in Musil and in Moritz, on every system.
//
// Included by music.h after Orchidea's search (orchidea::search_segments) and with scientific.h's pursuit (mp_run).
#pragma once

#include "orchidea.h"
#include "score_layout.h"
#include "db_query.h"
#include "../rng.h"
#include "../signals/morphology.h"

#include <climits>
#include <cmath>
#include <functional>
#include <map>
#include <random>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <vector>

namespace musil {
namespace granular {

using rng_t = std::mt19937_64;
inline double rand01(rng_t& g) { return rnd::uniform01(g); }

// --- a parameter's value: what a Musil value can be here (nil, a number, a name, a list of them) ---
struct pval {
    enum kind { NIL, NUM, NAME, LIST } k = NIL;
    double num = 0;
    std::string name;
    std::vector<pval> list;
    static pval none() { return {}; }
    static pval number(double d) { pval v; v.k = NUM; v.num = d; return v; }
    static pval text(const std::string& s) { pval v; v.k = NAME; v.name = s; return v; }
    static pval of(std::vector<pval> l) { pval v; v.k = LIST; v.list = std::move(l); return v; }
    static pval range(double a, double b) { return of({number(a), number(b)}); }
    bool is_nil() const { return k == NIL; }
    bool is_num() const { return k == NUM; }
    bool is_list() const { return k == LIST; }
    // Musil's truth: 0, nil and the empty list are false
    bool truthy() const { return k == NUM ? num != 0 : k == LIST ? !list.empty() : k == NAME; }
    // (str v)
    std::string str() const;
};
// a number as Musil prints it (put_double)
inline std::string num_text(double d) {
    std::ostringstream os;
    if (std::isfinite(d) && std::fabs(d) < 1e15 && d == std::floor(d)) os << (long long)d;
    else os << std::setprecision(15) << d;
    return os.str();
}
inline std::string pval::str() const {
    switch (k) {
        case NIL: return "nil";
        case NUM: return num_text(num);
        case NAME: return name;
        default: { std::string s = "("; for (size_t j = 0; j < list.size(); j++) s += (j ? " " : "") + list[j].str(); return s + ")"; }
    }
}
// The rules on values are written once, for any representation of a value (Tr: its traits): pval here, Musil's own
// values in music.h (env-at, draw-in, pick-from, value-at, numeric-range? are these functions)
// (numeric-range? v): a list of two numbers
template <class Tr, class V> inline bool numeric_range_t(const V& v) {
    return Tr::is_list(v) && Tr::size(v) == 2 && Tr::is_num(Tr::at(v, 0)) && Tr::is_num(Tr::at(v, 1));
}
// an envelope: a list of (time value) pairs, as (env ...) makes it (value-at's test)
template <class Tr, class V> inline bool is_env_t(const V& v) {
    if (!Tr::is_list(v) || Tr::size(v) == 0) return false;
    const V& h = Tr::at(v, 0);
    return Tr::is_list(h) && Tr::size(h) == 2 && Tr::is_num(Tr::at(h, 0)) && !numeric_range_t<Tr>(v);
}
// (env-at e t): an interpolated number or range, or a set chosen by the crossfade (a draw)
template <class Tr, class V> inline V env_at_t(const V& e, double t, rng_t& g) {
    if (!Tr::is_list(e) || Tr::size(e) == 0) throw std::runtime_error("env-at: an envelope is a list of (time value) pairs");
    size_t n = Tr::size(e);
    for (size_t k = 0; k < n; k++) {
        const V& q = Tr::at(e, k);
        if (!Tr::is_list(q) || Tr::size(q) != 2 || !Tr::is_num(Tr::at(q, 0))) throw std::runtime_error("env-at: a breakpoint is a time and a value");
    }
    auto time = [&](size_t k) { return Tr::num(Tr::at(Tr::at(e, k), 0)); };
    auto value = [&](size_t k) -> const V& { return Tr::at(Tr::at(e, k), 1); };
    if (t <= time(0)) return value(0);
    if (t >= time(n - 1)) return value(n - 1);
    size_t k = 0;
    while (k + 1 < n && t >= time(k + 1)) k++;
    const V& a = value(k); const V& b = value(k + 1);
    double u = (t - time(k)) / std::max(1e-9, time(k + 1) - time(k));
    if (Tr::is_num(a) && Tr::is_num(b)) return Tr::number(Tr::num(a) + u * (Tr::num(b) - Tr::num(a)));
    if (numeric_range_t<Tr>(a) && numeric_range_t<Tr>(b)) {
        double a0 = Tr::num(Tr::at(a, 0)), a1 = Tr::num(Tr::at(a, 1)), b0 = Tr::num(Tr::at(b, 0)), b1 = Tr::num(Tr::at(b, 1));
        return Tr::range(a0 + u * (b0 - a0), a1 + u * (b1 - a1));
    }
    return rand01(g) < u ? b : a;
}
// (value-at v t): an envelope's value at t, or the value itself
template <class Tr, class V> inline V value_at_t(const V& v, double t, rng_t& g) { return is_env_t<Tr>(v) ? env_at_t<Tr>(v, t, g) : v; }
// (draw-in v) of a list: a number drawn between its first and its last
template <class Tr, class V> inline double draw_range_t(const V& v, rng_t& g) {
    size_t n = Tr::size(v);
    if (n == 0) throw std::runtime_error("draw-in: an empty range");
    if (!Tr::is_num(Tr::at(v, 0)) || !Tr::is_num(Tr::at(v, n - 1))) throw std::runtime_error("draw-in: a range of numbers");
    double h = Tr::num(Tr::at(v, 0)), l = Tr::num(Tr::at(v, n - 1));
    return h + rand01(g) * (l - h);
}
// (pick-from set): an element drawn from a set, or the value itself
template <class Tr, class V> inline V pick_from_t(const V& v, rng_t& g) {
    if (!Tr::is_list(v)) return v;
    if (Tr::size(v) == 0) throw std::runtime_error("pick-from: an empty set");
    return Tr::at(v, (size_t)std::floor(rand01(g) * (double)Tr::size(v)));
}
struct pval_traits {
    static bool is_num(const pval& v) { return v.k == pval::NUM; }
    static double num(const pval& v) { return v.num; }
    static bool is_list(const pval& v) { return v.k == pval::LIST; }
    static size_t size(const pval& v) { return v.list.size(); }
    static const pval& at(const pval& v, size_t k) { return v.list[k]; }
    static pval number(double d) { return pval::number(d); }
    static pval range(double a, double b) { return pval::range(a, b); }
};
inline bool numeric_range(const pval& v) { return numeric_range_t<pval_traits>(v); }
inline bool is_env(const pval& v) { return is_env_t<pval_traits>(v); }
inline double number_of(const pval& v, const char* what) {
    if (!v.is_num()) throw std::runtime_error(std::string(what) + ": a number was expected, got " + v.str());
    return v.num;
}
inline pval env_at(const pval& e, double t, rng_t& g) { return env_at_t<pval_traits>(e, t, g); }
inline pval value_at(const pval& v, double t, rng_t& g) { return value_at_t<pval_traits>(v, t, g); }
inline double draw_in(const pval& v, rng_t& g) { return v.is_list() ? draw_range_t<pval_traits>(v, g) : number_of(v, "draw-in"); }
inline pval pick_from(const pval& v, rng_t& g) { return pick_from_t<pval_traits>(v, g); }
// (draw-duration v law): a duration drawn in a range by a law: 'uniform, 'log (log-uniform), or an exponent
// law: 0 uniform, 1 log, 2 an exponent
inline double draw_duration_in(double a, double b, int law, double exponent, rng_t& g) {
    double u = rand01(g);
    if (law == 1) return std::max(a, 1e-4) * std::pow(std::max(b, 1e-4) / std::max(a, 1e-4), u);
    if (law == 2) return a + (b - a) * std::pow(u, exponent);
    return a + u * (b - a);
}
inline double draw_duration(const pval& v, const pval& law, rng_t& g) {
    if (!v.is_list()) return number_of(v, "duration");
    if (v.list.empty()) throw std::runtime_error("duration: an empty range");
    double a = number_of(v.list.front(), "duration"), b = number_of(v.list.back(), "duration");
    return draw_duration_in(a, b, law.k == pval::NAME && law.name == "log" ? 1 : law.is_num() ? 2 : 0, law.num, g);
}
// (band-weigher lo hi weight tilt)'s weight of a pitch m in the band lo..hi (MIDI): 'weight the middle (1) or the edges
// (-1), 'tilt the top (1) or the bottom (-1)
inline double band_weight(double lo, double hi, double weight, double tilt, double m) {
    double span = std::max(1.0, hi - lo), u = std::max(0.0, std::min(1.0, (m - lo) / span));
    double centre = 1 - std::fabs(2 * u - 1);
    double w = (1 - std::fabs(weight)) + std::fabs(weight) * (weight > 0 ? centre : 1 - centre);
    double v = (1 - std::fabs(tilt)) + std::fabs(tilt) * (tilt > 0 ? u : 1 - u);
    return 0.01 + w * v;
}

// --- pitches, dynamics, gains, families ---
inline double midi_hz(double m) { return 440 * std::pow(2, (m - 69) / 12); }
inline double hz_midi(double f) { return 69 + 12 * std::log2(f / 440); }
// (pitch->number p): a MIDI number from a number or a name (-1: not a pitch)
inline double pitch_number(const pval& p) { return p.is_num() ? p.num : (double)pitch_to_midi(p.str()); }
// (harmonic-series fundamental n): the first n partials as MIDI pitches, rounded
inline std::vector<double> harmonic_series(double fundamental, int n) {
    std::vector<double> out;
    for (int k = 1; k <= n; k++) out.push_back(std::round(hz_midi((double)k * midi_hz(fundamental))));
    return out;
}
// (just-cents fundamental pitch n): the cents a tempered pitch is from the nearest of the first n partials (within a
// quarter tone), or none
inline bool just_cents(double fundamental, double pitch, int n, double& cents) {
    double f0 = midi_hz(fundamental), f = midi_hz(pitch), k = std::max(1.0, std::round(f / f0));
    if (k > n) return false;
    double c = 1200 * std::log2((k * f0) / f);
    if (std::fabs(c) < 50) { cents = std::round(c); return true; }
    return false;
}
using dbq::dynamics_scale;                                // ppp pp p mp mf f ff fff (db_query.h: the order the queries' ranges use)
// (level->dynamics x) (dynamics->level d) (level->gain x)
inline std::string level_dynamics(double x) { return dynamics_scale()[(size_t)std::max(0.0, std::min(7.0, std::round(7 * x)))]; }
inline double dynamics_level(const std::string& d) {
    const auto& s = dynamics_scale(); auto it = std::find(s.begin(), s.end(), d);
    return it == s.end() ? 0.5 : (double)(it - s.begin()) / 7;
}
inline double level_gain(double x) { return 0.25 + 0.75 * std::max(0.0, std::min(1.0, x)); }
// (instrument-family instr): winds brass percussion keyboard plucked strings, other (db_query.h: a query's family)
using dbq::instrument_family;
// (balance-gain balance instr): a balance is a list of (name decibels) pairs, by instrument or family
inline double balance_gain(const pval& balance, const std::string& instr) {
    if (balance.is_nil()) return 1;
    const pval *own = nullptr, *fam = nullptr; std::string family = instrument_family(instr);
    if (balance.is_list()) for (auto& kv : balance.list) {
        if (!kv.is_list() || kv.list.size() < 2) continue;
        if (!own && kv.list[0].str() == instr) own = &kv.list.back();
        if (!fam && kv.list[0].str() == family) fam = &kv.list.back();
    }
    double d = own ? number_of(*own, "balance") : fam ? number_of(*fam, "balance") : 0;
    return std::pow(10, d / 20);
}
// techniques short by nature, (technique-short? t)
inline bool technique_short(const std::string& t) {
    static const char* const shorts[] = {"pizz", "stac", "secco", "col-legno-batt", "slap", "key", "bartok", "snap", "tongue-ram", "sforz", "attack"};
    for (const char* w : shorts) if (t.find(w) != std::string::npos) return true;
    return false;
}

// --- the database as the granulator reads it: per instrument its available pitched sounds (db-index!) ---
struct dbindex {
    const std::vector<orchidea::entry>* entries = nullptr;     // every entry of the database (index: its place)
    struct slot {
        std::vector<int> all;                                   // the available pitched entries, in the database's order
        std::map<std::string, std::vector<int>> by;             // by "dyn|tech"
        std::map<int, std::vector<int>> by_midi;
    };
    std::map<std::string, slot> slots;
    const orchidea::entry& at(int k) const { return (*entries)[(size_t)k]; }
    const slot* find(const std::string& instr) const { auto it = slots.find(instr); return it == slots.end() ? nullptr : &it->second; }
};
// the index of the entries whose sound is on disk (available, one per entry) and whose pitch is known
inline dbindex make_index(const std::vector<orchidea::entry>& entries, const std::vector<char>& available) {
    dbindex ix; ix.entries = &entries;
    for (size_t k = 0; k < entries.size(); k++) {
        const auto& e = entries[k];
        if (e.midi < 0 || k >= available.size() || !available[k]) continue;
        auto& s = ix.slots[e.instr];
        s.all.push_back((int)k);
        s.by[e.dyn + "|" + e.tech].push_back((int)k);
        s.by_midi[e.midi].push_back((int)k);
    }
    return ix;
}
// what a run asks of the index again and again, computed once a run: an instrument's pitches with a technique (any:
// empty) and a dynamics (any: empty), sorted (db-pitches-at)
struct pitch_cache {
    const dbindex& ix;
    std::map<std::string, std::vector<int>> memo;
    explicit pitch_cache(const dbindex& x) : ix(x) {}
    const std::vector<int>& pitches(const std::string& instr, const std::string* tech, const std::string* dyn) {
        std::string key = instr + "|" + (tech ? "t" + *tech : "*") + "|" + (dyn ? "d" + *dyn : "*");
        auto it = memo.find(key);
        if (it != memo.end()) return it->second;
        std::vector<int> ps;
        if (const auto* s = ix.find(instr))
            for (int k : s->all) { const auto& e = ix.at(k); if ((!tech || e.tech == *tech) && (!dyn || e.dyn == *dyn)) ps.push_back(e.midi); }
        std::sort(ps.begin(), ps.end()); ps.erase(std::unique(ps.begin(), ps.end()), ps.end());
        return memo[key] = ps;
    }
    // (instrument-has? db instr tech dyn lo hi)
    bool has(const std::string& instr, const std::string& tech, const std::string* dyn, double lo, double hi) {
        for (int m : pitches(instr, &tech, dyn)) if (m >= lo && m <= hi) return true;
        return false;
    }
};
// (db-candidates db instr dyn tech): the entries at these dynamics and technique; else with the technique; else all
inline std::vector<int> candidates(const dbindex& ix, const std::string& instr, const std::string* dyn, const std::string& tech) {
    const auto* s = ix.find(instr);
    if (!s) return {};
    if (dyn) { auto it = s->by.find(*dyn + "|" + tech); if (it != s->by.end()) return it->second; }
    std::vector<int> same;
    for (int k : s->all) if (ix.at(k).tech == tech) same.push_back(k);
    return same.empty() ? s->all : same;
}
// (note db instr pitch dyn tech): the entry a note takes, and its shift: the sample at the pitch with the technique
// (at the dynamics asked for, else the nearest recorded), else the nearest pitch transposed
struct picked { int entry = -1; int midi = 0, shift = 0; };
inline picked pick_note(const dbindex& ix, const std::string& instr, double pitch, const std::string& dyn, const std::string& tech) {
    int want = (int)std::round(pitch);
    if (want < 0) throw std::runtime_error("note: not a pitch: " + num_text(pitch));
    std::vector<int> same = candidates(ix, instr, &dyn, tech);
    if (same.empty()) throw std::runtime_error("note: no sound of " + instr + " on disk in this database");
    std::vector<int> at_pitch, exact;
    for (int k : candidates(ix, instr, nullptr, tech)) if (ix.at(k).midi == want) at_pitch.push_back(k);
    for (int k : same) if (ix.at(k).midi == want) exact.push_back(k);
    int e;
    if (!exact.empty()) e = exact[0];
    else if (!at_pitch.empty()) {
        double want_level = dynamics_level(dyn), best = 1e300; e = at_pitch[0];
        for (int k : at_pitch) { double d = std::fabs(dynamics_level(ix.at(k).dyn) - want_level); if (d < best) { best = d; e = k; } }
    } else {
        double best = 1e300; e = same[0];
        for (int k : same) { double d = std::fabs((double)(ix.at(k).midi - want)); if (d < best) { best = d; e = k; } }
    }
    return {e, want, want - ix.at(e).midi};
}

// --- what a run writes ---
struct note {
    double at = 0, dur = 0;
    int entry = -1, midi = 0, shift = 0, player = 0;
    std::string instr, pitch, dyn, tech, label;
    double cents = 0, gain = 1;
    std::vector<double> atom;                                   // a morphological note: its sound in the pursuit's space
};
inline note make_note(const dbindex& ix, const std::string& instr, double pitch, const std::string& dyn, const std::string& tech) {
    picked p = pick_note(ix, instr, pitch, dyn, tech);
    note n; n.entry = p.entry; n.midi = p.midi; n.shift = p.shift; n.instr = instr; n.pitch = midi_to_pitch(p.midi);
    n.dyn = dyn; n.tech = tech; n.label = instr + " " + n.pitch + " " + dyn;
    return n;
}
struct unplayable { double at = 0; pval styles, reg; bool strict = false; std::string dyn; };
struct player_report { std::vector<std::string> instrs; int notes = 0; double busy = 0; std::string seat; };
struct report {
    double seconds = 0; int due = 0, events = 0, notes = 0, skipped_busy = 0, skipped_unplayable = 0, substituted = 0;
    double saturation = 0;
    std::vector<unplayable> unplayables;                        // what the orchestra cannot play, once each
    std::vector<player_report> players;
};

// --- the orchestra: its players and what each keeps between notes (and between runs, with 'continue) ---
struct player {
    std::vector<std::string> instrs;                            // one, or an ossia's alternatives
    int group = -1;                                             // a paired group, or -1
    double busy_until = 0, seat_since = 0;
    bool has_last_pitch = false, has_last_tech = false, seated = false;
    double last_pitch = 0;
    std::string last_tech, seat;
    std::vector<double> starts, ends;                           // the bookings ('continue)
    void rest() { busy_until = 0; seat_since = 0; has_last_pitch = has_last_tech = seated = false; last_tech.clear(); seat.clear(); starts.clear(); ends.clear(); }
};
// (orchestra spec): a name a player, "Fl|Picc" an ossia, a list a group of players paired
inline std::vector<std::string> ossia(const std::string& item) {
    std::vector<std::string> out; std::string cur;
    for (char c : item) { if (c == '|') { out.push_back(cur); cur.clear(); } else cur += c; }
    out.push_back(cur);
    return out;
}

// --- the target of a morphological orchestration: a sound analysed into curves ---
struct target {
    double sr = 44100, seconds = 0;
    int block = 2048, hop = 1024, fine_block = 8192;
    std::vector<double> times, attacks, density, centroid, spread, low, loudness;
    morph::spectra spectra, fine;
    double loud_max() const { return loudness.empty() ? -100 : *std::max_element(loudness.begin(), loudness.end()); }
};
struct analysis_options { double threshold = 2, window = 1; int fine = 8192; };
// (target-analyse-with x sr block hop opts)
inline target analyse(const std::vector<double>& x, double sr, int block, int hop, const analysis_options& o = {}) {
    if (!(sr > 0)) throw std::runtime_error("target-analyse: the sample rate must be > 0");
    if (block < 2 || hop < 1 || o.fine < 2) throw std::runtime_error("target-analyse: the block (2 at least) and the hop (1 at least)");
    double frames = (double)x.size() / hop + 2;
    if (frames * ((double)block / 2 + (double)o.fine / 2) > 3e8)
        throw std::runtime_error("target-analyse: too many frames to hold (" + num_text(std::floor(frames)) + "): a larger hop or a shorter sound");
    target g; g.sr = sr; g.block = block; g.hop = hop; g.fine_block = o.fine;
    g.spectra = morph::frame_spectra(x, (size_t)block, (size_t)hop, (size_t)block / 2);
    size_t n = g.spectra.size();
    g.fine = morph::frame_spectra(x, (size_t)o.fine, (size_t)hop, (size_t)o.fine / 2);
    while (g.fine.size() < n) g.fine.push_back(g.fine.back());
    g.fine.resize(n);
    double step = (double)hop / sr;
    g.seconds = (double)x.size() / sr;
    g.attacks = morph::flux_peaks(x, sr, 1024, 256, o.threshold, 21);
    morph::register_curves reg = morph::register_curve(g.spectra, sr, block);
    auto take = [n](std::vector<double> v) { if (v.size() > n) v.resize(n); return v; };
    for (size_t k = 0; k < n; k++) g.times.push_back((double)k * step);
    g.density = take(morph::peak_density(g.attacks, g.seconds, o.window, step));
    g.centroid = morph::moving_median(take(reg.centroid), 5);
    g.spread = morph::moving_median(take(reg.spread), 5);
    g.low = morph::moving_median(take(reg.low), 5);
    g.loudness = morph::moving_median(take(morph::loudness_curve(x, sr, (size_t)hop)), 5);
    return g;
}
// (target-frame target t)
inline size_t target_frame(const target& g, double t) {
    double k = std::floor(t * (g.sr / g.hop));
    return (size_t)std::max(0.0, std::min((double)g.spectra.size() - 1, k));
}
// (target-level target t range): the loudness at t as a level 0..1 (1 at the loudest, 0 range dB under it)
inline double target_level(const target& g, double t, double range) {
    if (g.loudness.empty()) return 0;
    return std::max(0.0, std::min(1.0, 1 + (g.loudness[std::min(target_frame(g, t), g.loudness.size() - 1)] - g.loud_max()) / range));
}
// (frames-mean spectra k0 k1 n): the frames k0..k1 averaged, their first n bins
inline std::vector<double> frames_mean(const morph::spectra& s, size_t k0, size_t k1, size_t n) {
    if (k1 >= s.size() || k0 > k1) throw std::runtime_error("target: fewer frames than its curves (not a target-analyse record)");
    std::vector<double> acc(s[k0].begin(), s[k0].begin() + (long)std::min(n, s[k0].size()));
    for (size_t k = k0 + 1; k <= k1; k++) {
        if (s[k].size() < acc.size()) throw std::runtime_error("target: the frames differ in length");
        for (size_t j = 0; j < acc.size(); j++) acc[j] = acc[j] + s[k][j];
    }
    double c = (double)(1 + k1 - k0);
    for (auto& v : acc) v = v / c;
    return acc;
}
// (curve->env times values step maker): a breakpoint every step seconds, the values over the step averaged
inline pval curve_env(const std::vector<double>& times, const std::vector<double>& values, double step, const std::function<pval(double)>& maker) {
    pval e = pval::of({});
    if (times.empty()) return e;
    if (!(step > 0)) throw std::runtime_error("target-envelopes: the step must be > 0");
    double end = times.back();
    for (double t = 0; t <= end; t = t + step) {
        double s = 0; int n = 0;
        for (size_t k = 0; k < times.size(); k++) if (times[k] >= t && times[k] < t + step) { s += values[k]; n++; }
        if (n > 0) e.list.push_back(pval::of({pval::number(t), maker(s / n)}));
    }
    return e;
}
// (target-envelopes target opts): the granulator's density, register and dynamics read off the target
struct envelope_options { double step = 0.25, width = 1, min_density = 0.5, density_scale = 1, range = 40; };
struct envelopes { pval density, reg, dynamics; };
inline envelopes target_envelopes(const target& g, const envelope_options& o) {
    envelopes out; size_t n = g.times.size();
    if (g.density.size() < n || g.centroid.size() < n || g.spread.size() < n || g.low.size() < n || g.loudness.size() < n)
        throw std::runtime_error("target-envelopes: a curve shorter than the times");
    out.density = curve_env(g.times, g.density, o.step, [&](double v) { return pval::number(std::max(o.min_density, o.density_scale * v)); });
    auto oct = [](double m) { return (m - 12) / 12; };
    std::vector<double> lo(n), hi(n), dyn(n);
    double mx = g.loud_max();
    for (size_t k = 0; k < n; k++) {
        double w = (o.width * 12) * g.spread[k];
        double a = g.centroid[k] - w;
        lo[k] = g.low[k] < a ? g.low[k] : a;
        hi[k] = g.centroid[k] + w;
        dyn[k] = std::max(0.0, std::min(1.0, 1 + (g.loudness[k] - mx) / o.range));
    }
    pval elo = curve_env(g.times, lo, o.step, [&](double v) { return pval::number(std::max(0.0, oct(v))); });
    pval ehi = curve_env(g.times, hi, o.step, [&](double v) { return pval::number(std::min(8.5, oct(v))); });
    out.reg = pval::of({});
    for (size_t k = 0; k < elo.list.size() && k < ehi.list.size(); k++) {
        double a = elo.list[k].list[1].num, b = ehi.list[k].list[1].num;
        out.reg.list.push_back(pval::of({elo.list[k].list[0], pval::range(a, std::max(0.5 + a, b))}));
    }
    out.dynamics = curve_env(g.times, dyn, o.step, [](double v) { return pval::number(v); });
    return out;
}
// (target-peaks target t n): the frequencies of the n strongest peaks of the fine spectrum over the next 0.1 s
inline std::vector<double> target_peaks(const target& g, double t, size_t n) {
    std::vector<double> out;
    if (g.fine.empty()) return out;
    double bin = g.sr / g.fine_block;
    std::vector<double> m = frames_mean(g.fine, target_frame(g, t), target_frame(g, t + 0.1), (size_t)g.fine_block / 2);
    if (m.empty()) return out;
    double top = *std::max_element(m.begin(), m.end());
    if (top <= 0) return out;
    std::vector<size_t> ks;
    for (size_t k = 1; k + 1 < m.size(); k++)
        if (m[k] > m[k - 1] && m[k] > m[k + 1] && m[k] > 0.05 * top && (double)k * bin > 30) ks.push_back(k);
    std::stable_sort(ks.begin(), ks.end(), [&](size_t a, size_t b) { return (0 - m[a]) < (0 - m[b]); });
    if (ks.size() > n) ks.resize(n);
    for (size_t k : ks) {
        double a = m[k - 1], b = m[k], c = m[k + 1], d = (a + c) - 2 * b;
        out.push_back(((double)k + (d == 0 ? 0 : (a - c) / (2 * d))) * bin);
    }
    return out;
}
// an entry's sound in the pursuit's space (the square root of its spectrum), and the frequency it sounds at (the peak
// of its spectrum nearest its pitch, within a semitone, by a parabola; the nominal one when there is none): what a run
// computes once an entry
struct entry_cache {
    const dbindex& ix;
    std::map<int, std::vector<double>> atoms;
    std::map<int, double> norms;
    std::map<std::pair<int, double>, double> f0s;
    explicit entry_cache(const dbindex& x) : ix(x) {}
    const std::vector<double>& atom(int k) {
        auto it = atoms.find(k);
        if (it != atoms.end()) return it->second;
        const auto& f = ix.at(k).features; std::vector<double> a(f.size());
        for (size_t j = 0; j < f.size(); j++) a[j] = std::sqrt(std::max((double)f[j], 0.0));
        return atoms[k] = a;
    }
    double norm(int k) {
        auto it = norms.find(k);
        if (it != norms.end()) return it->second;
        const auto& a = atom(k); double s = 0; for (double v : a) s += v * v;
        return norms[k] = std::sqrt(s);
    }
    double f0(int k, double sr, double block) {
        auto key = std::make_pair(k, sr / block);
        auto it = f0s.find(key);
        if (it != f0s.end()) return it->second;
        return f0s[key] = entry_f0(ix.at(k), sr, block);
    }
    static double entry_f0(const orchidea::entry& e, double sr, double block) {
        const auto& m = e.features;
        double nominal = midi_hz(e.midi), bin = nominal / (sr / block);
        double lo = std::max(1.0, std::floor(bin / 1.06)), hi = std::min((double)m.size() - 2, std::ceil(bin * 1.06));
        long best = -1;
        for (long k = (long)lo; (double)k <= hi; k++)
            if (m[(size_t)k] > m[(size_t)k - 1] && m[(size_t)k] >= m[(size_t)k + 1] && (best < 0 || m[(size_t)k] > m[(size_t)best])) best = k;
        if (best < 0) return nominal;
        double a = m[(size_t)best - 1], b = m[(size_t)best], c = m[(size_t)best + 1], d = (a + c) - 2 * b;
        return ((double)best + (d == 0 ? 0 : (a - c) / (2 * d))) * (sr / block);
    }
};
// (atom-persistence target features t persist max-length): how long the target goes on holding a sound from t (its
// projection on the sound's spectrum at least persist of its peak within the first 0.1 s), in seconds
template <class F>
inline double atom_persistence(const target& g, const std::vector<F>& features, double t, double persist, double max_length) {
    if (g.spectra.empty()) return 0;
    double step = (double)g.hop / g.sr;
    size_t k0 = target_frame(g, t), k1 = target_frame(g, t + 0.1), n = features.size();
    auto dot = [&](size_t k) {
        const auto& s = g.spectra[k];
        if (s.size() < n) throw std::runtime_error("atom-persistence: the target's spectra are shorter than the features");
        double v = 0; for (size_t j = 0; j < n; j++) v += (double)features[j] * s[j];
        return v;
    };
    double peak = 0; size_t k = k0;
    for (; k <= k1; k++) peak = std::max(peak, dot(k));
    if (peak <= 1e-12) return 0;
    double cap = std::min((double)g.spectra.size(), (double)k0 + std::ceil(max_length / step));
    while ((double)k < cap && dot(k) >= persist * peak) k++;
    return (double)(k - k0) * step;
}

// --- the parameters of a run: a record from names to values (any may be an envelope) ---
struct params {
    std::map<std::string, pval> m;
    const pval* get(const std::string& key) const { auto it = m.find(key); return it == m.end() ? nullptr : &it->second; }
    bool has(const std::string& key) const { return m.count(key) > 0; }
    pval opt(const std::string& key, const pval& def) const { const pval* v = get(key); return v ? *v : def; }
    double num(const std::string& key, double def) const { const pval* v = get(key); return v ? number_of(*v, key.c_str()) : def; }
    // (param-at params key t default)
    pval at(const std::string& key, double t, const pval& def, rng_t& g) const { return value_at(opt(key, def), t, g); }
    void set(const std::string& key, pval v) { m[key] = std::move(v); }
};
// the Markov method's table: a pitch and the pitches that followed it, in the order they were learnt
using markov_table = std::vector<std::pair<double, std::vector<double>>>;

// --- the morphological decision at an event ---
struct choice { int player = -1; note n; double life = 0; };       // player: its index in the orchestra
// (target-dictionary db peaks players lo hi dyn styles): for every player the sounds of its instruments at the target's
// peaks (within 60 cents) inside lo..hi, at the dynamics when the instrument has it, with one of the styles (all: none)
struct atom_of { int player; int entry; };
inline std::vector<atom_of> target_dictionary(const dbindex& ix, const std::vector<double>& peaks, const std::vector<int>& free,
                                              const std::vector<player>& players, double lo, double hi, const std::string& dyn,
                                              const std::vector<std::string>* styles) {
    std::vector<int> keys;
    for (double f : peaks) {
        double m = hz_midi(f);
        for (int d = -1; d <= 1; d++) {
            double p = std::round(m) + d;
            if (std::fabs(m - p) <= 0.6 && p >= lo && p <= hi && std::find(keys.begin(), keys.end(), (int)p) == keys.end()) keys.push_back((int)p);
        }
    }
    std::vector<atom_of> out;
    for (int q : free) for (const auto& i : players[(size_t)q].instrs) {
        const auto* s = ix.find(i);
        if (!s) continue;
        std::vector<int> ok;
        for (int p : keys) {
            auto it = s->by_midi.find(p);
            if (it == s->by_midi.end()) continue;
            for (int k : it->second) if (!styles || std::find(styles->begin(), styles->end(), ix.at(k).tech) != styles->end()) ok.push_back(k);
        }
        std::vector<int> same;
        for (int k : ok) if (ix.at(k).dyn == dyn) same.push_back(k);
        for (int k : same.empty() ? ok : same) out.push_back({q, k});
    }
    return out;
}
// the parameters the decision reads, once a run
struct decision_options {
    double range = 40, floor = 0.1, voices = 2, min_gain = 0.2, min_length = 0.1, persist = 0.5, max_length = 8, short_life = 0.4;
    bool has_styles = false; std::vector<std::string> styles;
    bool genetic = false;
    int population = 60, epochs = 40, pursuit = 0, patience = 20;
    double sparsity = 0.1, mutation = 0.02, positive = 0.5, negative = 10;
};
// (target-note-of ...): the note made of the entry chosen at t, as long as the target holds its sound, a short
// technique of the same sound when that is short, at the target's level, tuned to the nearest peak
inline choice target_note_of(const dbindex& ix, entry_cache& ec, const target& g, double t, int player, int e, const std::vector<double>& residual,
                             const std::vector<double>& peaks, double level, const std::string& dyn, const decision_options& o) {
    choice c; c.player = player;
    c.life = std::max(o.min_length, atom_persistence(g, ix.at(e).features, t, o.persist, o.max_length));
    int entry = e;
    if (c.life < o.short_life && !technique_short(ix.at(e).tech)) {
        const auto* s = ix.find(ix.at(e).instr);
        std::vector<int> shorts;
        if (s) for (int b : s->all) {
            const auto& be = ix.at(b);
            if (be.midi == ix.at(e).midi && technique_short(be.tech) && (!o.has_styles || std::find(o.styles.begin(), o.styles.end(), be.tech) != o.styles.end())) shorts.push_back(b);
        }
        if (!shorts.empty()) {
            double best = 1e300;
            for (int b : shorts) {
                const auto& a = ec.atom(b); double d = 0; size_t n = std::min(a.size(), residual.size());
                for (size_t j = 0; j < n; j++) d += residual[j] * a[j];
                double v = 0 - d / std::max(1e-9, ec.norm(b));
                if (v < best) { best = v; entry = b; }
            }
        }
    }
    const auto& en = ix.at(entry);
    c.n = make_note(ix, en.instr, en.midi, en.dyn, en.tech);
    c.n.dyn = dyn;
    c.n.label = en.instr + " " + c.n.pitch + " " + dyn;
    c.n.gain = level_gain(level);
    c.n.atom = ec.atom(entry);
    double f0 = ec.f0(entry, g.sr, g.block);
    c.n.cents = 0;
    if (!peaks.empty()) {
        double nearest = peaks[0], bd = std::fabs(peaks[0] - f0);
        for (double p : peaks) if (std::fabs(p - f0) < bd) { bd = std::fabs(p - f0); nearest = p; }
        double cents = 1200 * std::log2(std::max(nearest, 1.0) / f0);
        c.n.cents = std::fabs(cents) < 50 ? std::round(cents) : 0;
    }
    return c;
}
// (target-residual target t sounding n): the target's spectrum at t (its first n bins over the next 0.1 s, in the
// pursuit's space) less what the sounding notes account for (their atoms refitted together, non-negative)
inline std::pair<std::vector<double>, std::vector<double>> target_residual(const target& g, double t, const std::vector<const note*>& sounding, size_t n) {
    std::vector<double> frame = frames_mean(g.spectra, target_frame(g, t), target_frame(g, t + 0.1), n);
    for (auto& v : frame) v = std::sqrt(std::max(v, 0.0));
    if (sounding.empty()) return {frame, frame};
    mat D(sounding.size(), frame.size());
    for (size_t k = 0; k < sounding.size(); k++) {
        if (sounding[k]->atom.size() != frame.size()) throw std::runtime_error("target: the sounds' features and the target's spectra differ in length");
        for (size_t j = 0; j < frame.size(); j++) D(k, j) = sounding[k]->atom[j];
    }
    mp_result fit = mp_run(frame, D, (long)sounding.size(), 0, true, true);
    for (auto& v : fit.residual) v = std::max(0.0, v);
    return {fit.residual, frame};
}
inline double norm_of(const std::vector<double>& v) { double s = 0; for (double x : v) s += x * x; return std::sqrt(s); }
// (target-notes ...) and (target-notes-genetic ...): the notes to start at t on the free players
inline std::vector<choice> target_notes(const dbindex& ix, entry_cache& ec, const target& g, double t, const std::vector<int>& free,
                                        const std::vector<player>& players, const std::vector<const note*>& sounding, const std::function<pval()>& register_now,
                                        const decision_options& o, rng_t& rng, const std::function<void(const std::string&)>& trace,
                                        const std::function<bool()>& alive = {}) {
    std::vector<choice> out;
    if (g.loudness.empty() || g.spectra.empty()) return out;
    if (g.loudness[std::min(target_frame(g, t), g.loudness.size() - 1)] < g.loud_max() - o.range) return out;   // silence
    double level = target_level(g, t, o.range);
    pval reg_now = register_now();
    if (!numeric_range(reg_now)) throw std::runtime_error("orchestrate-morphological: the register is a range of octaves");
    double lo = 12 * (1 + reg_now.list[0].num), hi = 12 * (2 + reg_now.list[1].num) - 1;
    std::string dyn = level_dynamics(level);
    std::vector<double> peaks = target_peaks(g, t, 24);
    const std::vector<std::string>* styles = o.has_styles ? &o.styles : nullptr;
    std::vector<atom_of> atoms = target_dictionary(ix, peaks, free, players, lo, hi, dyn, styles);
    if (atoms.empty()) atoms = target_dictionary(ix, peaks, free, players, 0, 127, dyn, styles);
    if (atoms.empty()) return out;
    auto rf = target_residual(g, t, sounding, ix.at(atoms[0].entry).features.size());
    const std::vector<double>& frame = rf.second;
    const std::vector<double>& residual = norm_of(rf.first) < o.floor * norm_of(frame) ? frame : rf.first;
    if (norm_of(frame) <= 1e-9) return out;
    if (!o.genetic) {
        mat D(atoms.size(), residual.size());
        for (size_t k = 0; k < atoms.size(); k++) {
            const auto& a = ec.atom(atoms[k].entry);
            if (a.size() != residual.size()) throw std::runtime_error("mp: the atoms must have the length of the signal");
            for (size_t j = 0; j < a.size(); j++) D(k, j) = a[j];
        }
        double voices = std::max(1.0, std::min((double)free.size(), o.voices));
        mp_result r = mp_run(residual, D, (long)voices, 0.05, true, false);
        double rnorm = norm_of(residual);
        std::vector<int> taken;
        for (size_t j = 0; j < r.atoms.size(); j++) {
            const atom_of& a = atoms[r.atoms[j]]; double w = r.weights[j];
            double explains = (w * ec.norm(a.entry)) / std::max(1e-9, rnorm);
            if (trace) {
                const auto& e = ix.at(a.entry);
                trace("   " + num_text(t) + " " + e.instr + " " + e.pitch + " " + e.tech + " weight " + num_text(std::round(w * 1000) / 1000) +
                      " explains " + num_text(std::round(explains * 100) / 100));
            }
            if (std::find(taken.begin(), taken.end(), a.player) == taken.end() && w > 0 && (out.empty() || explains >= o.min_gain)) {
                taken.push_back(a.player);
                out.push_back(target_note_of(ix, ec, g, t, a.player, a.entry, residual, peaks, level, dyn, o));
            }
        }
        return out;
    }
    // the genetic decision: Orchidea's search over the free players' sounds (each player its own instrument "pK")
    std::vector<int> who_players; std::vector<orchidea::entry> local; std::vector<atom_of> who;
    for (const auto& a : atoms) {
        auto it = std::find(who_players.begin(), who_players.end(), a.player);
        size_t k = (size_t)(it - who_players.begin());
        if (it == who_players.end()) { who_players.push_back(a.player); k = who_players.size() - 1; }
        orchidea::entry e = ix.at(a.entry);
        e.index = (int)local.size(); e.instr = "p" + std::to_string(k);
        local.push_back(std::move(e)); who.push_back(a);
    }
    std::vector<std::string> names;
    for (size_t k = 0; k < who_players.size(); k++) names.push_back("p" + std::to_string(k));
    std::vector<double> lin(residual.size());
    for (size_t j = 0; j < lin.size(); j++) { double v = std::max(0.0, residual[j]); lin[j] = std::pow(v, 2); }
    double s = 0; for (double v : lin) s += v;
    double mean = s / (double)std::max<size_t>(1, lin.size());
    double var = 0; for (double v : lin) var += std::pow(v - mean, 2);
    double sd = std::sqrt(var / (double)std::max<size_t>(1, lin.size()));
    orchidea::segment seg; seg.start = t; seg.length = 1;
    for (double v : lin) seg.features.push_back(sd > 0 ? (float)((v - mean) / sd) : (float)(0 * v));
    orchidea::search_options so;
    so.p.pop_size = o.population; so.p.max_epochs = o.epochs; so.p.sparsity = o.sparsity; so.p.mutation_rate = o.mutation;
    so.p.positive = o.positive; so.p.negative = o.negative; so.p.pursuit = o.pursuit; so.p.patience = o.patience;
    so.p.max_solutions = 1; so.sustain = false;
    so.p.seed = (unsigned)(uint64_t)(int64_t)std::floor(rand01(rng) * 1e9);
    orchidea::search_result r = orchidea::search_segments(local, names, {seg}, so, [&](size_t, size_t, int, int) { return !alive || alive(); });
    if (r.stopped) return out;
    if (r.segments.empty() || r.segments[0].solutions.empty()) return out;
    const auto& sol = r.segments[0].solutions[0].second;
    for (size_t j = 0; j < sol.size() && j < r.segments[0].players.size(); j++) {
        if (sol[j] < 0) continue;
        const atom_of& a = who[(size_t)sol[j]];
        out.push_back(target_note_of(ix, ec, g, t, a.player, a.entry, residual, peaks, level, dyn, o));
    }
    return out;
}

// --- the granulator ---
struct run_result {
    std::vector<note> notes;
    report rep;
    std::vector<std::string> messages;                          // what the run says (the unplayable, once each)
    bool stopped = false;                                       // progress asked to stop
};
struct run_hooks {
    std::function<bool(double)> progress;                       // the fraction done; false stops the run
    std::function<void(const std::string&)> trace;              // the morphological pursuit's atoms ('trace)
};
// (granulate db orch secs params): the process over secs seconds with these players. carry ('continue): the players'
// own state goes in and comes out (their bookings, last pitches, seats); otherwise fresh copies at rest play. tgt: the
// target of a morphological orchestration ('method 'target), table: the Markov method's
inline run_result granulate(const dbindex& ix, std::vector<player>& orch, double secs, const params& p, const target* tgt,
                            const markov_table* table, rng_t& rng, const run_hooks& hooks = {}) {
    run_result res;
    bool carry = p.opt("continue", pval::number(0)).truthy();
    std::vector<player> fresh;
    if (!carry) { fresh = orch; for (auto& q : fresh) q.rest(); }
    std::vector<player>& players = carry ? orch : fresh;
    size_t np = players.size();
    double start = carry ? p.num("start", 0) : 0;
    std::string method = p.opt("method", pval::text("random")).str();
    bool is_target = method == "target";
    if (is_target && !tgt) throw std::runtime_error("orchestrate-morphological: no target");
    pitch_cache pc(ix);
    entry_cache ec(ix);
    // what each player does in this run
    std::vector<double> till(np, 1e9), since(np, 0);
    std::vector<long> current(np, -1);                          // the note it is playing (the morphological steal shortens it)
    std::vector<int> st_notes(np, 0); std::vector<double> st_secs(np, 0);
    std::vector<int> groups;
    bool has_loose = false;
    for (auto& q : players) { if (q.group >= 0) { if (std::find(groups.begin(), groups.end(), q.group) == groups.end()) groups.push_back(q.group); } else has_loose = true; }
    const pval* gd = p.get("group-density");
    if (gd && gd->is_nil()) gd = nullptr;
    if (gd && (!gd->is_list() || gd->list.empty())) throw std::runtime_error("group-density: one density per group, a list");
    bool strict = p.opt("dynamics-strict", pval::number(0)).truthy();
    pval balance = p.opt("balance", pval::none());
    bool just = p.opt("just", pval::number(0)).truthy();
    double fundamental = pitch_number(p.opt("fundamental", pval::text("C2")));
    decision_options dopt;
    if (is_target) {
        dopt.range = p.num("range", 40); dopt.floor = p.num("floor", 0.1); dopt.voices = p.num("voices", 2); dopt.min_gain = p.num("min-gain", 0.2);
        dopt.min_length = p.num("min-length", 0.1); dopt.persist = p.num("persist", 0.5); dopt.max_length = p.num("max-length", 8);
        dopt.short_life = p.num("short", 0.4);
        if (const pval* s = p.get("styles"); s && !s->is_nil()) {
            dopt.has_styles = true;
            if (s->is_list()) for (auto& x : s->list) dopt.styles.push_back(x.str()); else dopt.styles.push_back(s->str());
        }
        dopt.genetic = p.opt("decision", pval::text("pursuit")).str() == "genetic";
        if (const pval* gp = p.get("genetic"); gp && gp->is_list()) {
            auto num = [&](const char* key, double def) {
                for (auto& kv : gp->list) if (kv.is_list() && kv.list.size() == 2 && kv.list[0].str() == key) return number_of(kv.list[1], key);
                return def;
            };
            auto whole = [](double v) { return (int)std::max(-1e9, std::min(1e9, v)); };
            dopt.population = whole(num("population", 60)); dopt.epochs = whole(num("epochs", 40)); dopt.sparsity = num("sparsity", 0.1);
            dopt.mutation = num("mutation", 0.02); dopt.positive = num("positive", 0.5); dopt.negative = num("negative", 10);
            dopt.pursuit = whole(num("pursuit", 0)); dopt.patience = whole(num("patience", 20));
        }
    }
    bool steal = p.opt("steal", pval::number(1)).truthy();
    bool has_duration = p.has("duration");
    std::map<double, int> covered;                              // the cluster method: how often each pitch has sounded
    std::vector<std::string> unplayable_keys;
    double t = 0, phase = 0;
    long voice_k = 0, events_seen = 0;
    auto in_list = [](const std::vector<double>& v, double x) { return std::find(v.begin(), v.end(), x) != v.end(); };
    while (t < secs) {
        if (hooks.progress && !hooks.progress(secs > 0 ? t / secs : 1)) { res.stopped = true; return res; }
        if (++events_seen > 100000000 || res.notes.size() > 5000000)
            throw std::runtime_error("orchestrate-granular: more than five million notes or a hundred million events: a lower density or a shorter time");
        double T = start + t;
        // the rate: 'density, or one rate per paired group with 'group-density
        std::vector<double> rates;
        if (gd) for (int g : groups) rates.push_back(std::max(0.0, draw_in(value_at(gd->list[(size_t)std::min<long>(g, (long)gd->list.size() - 1)], t, rng), rng)));
        double loose = (!gd || has_loose) ? std::max(0.0, draw_in(p.at("density", t, pval::range(1, 1), rng), rng)) : 0;
        double rsum = 0; for (double r : rates) rsum += r;
        double rate = std::max(0.001, gd ? loose + rsum : loose);
        double step = 1 / rate <= 1 ? 1 / rate : 0.25;
        bool due = true;
        if (1 / rate > 1) { phase = phase + 0.25 * rate; due = phase >= 1; if (due) phase = phase - 1; }
        bool has_gsel = false; int gsel = 0;
        if (due && gd) {
            double u = rand01(rng) * rate;
            if (u < loose) { has_gsel = true; gsel = -1; }
            double acc = loose;
            for (size_t j = 0; !has_gsel && j < groups.size(); j++) { acc = acc + rates[j]; if (u < acc) { has_gsel = true; gsel = groups[j]; } }
            if (!has_gsel) { has_gsel = true; gsel = groups.empty() ? -1 : groups.back(); }
        }
        pval reg = p.at("register", t, pval::range(3, 5), rng);
        if (!numeric_range(reg)) throw std::runtime_error("register: a range of octaves (list lo hi)");
        double lo = 12 * (1 + reg.list[0].num), hi = 12 * (2 + reg.list[1].num) - 1;
        if (!std::isfinite(lo) || !std::isfinite(hi) || std::fabs(lo) > 1e6 || std::fabs(hi) > 1e6) throw std::runtime_error("register: octaves, finite numbers");
        pval dv_dur = p.at("duration", t, pval::range(0.2, 1), rng);
        pval dv_law = p.at("duration-law", t, pval::text("uniform"), rng);
        double dur = draw_duration(dv_dur, dv_law, rng);
        pval style_set = p.at("styles", t, pval::of({pval::text("ord")}), rng);
        if (!style_set.is_list()) style_set = pval::of({style_set});
        pick_from(style_set, rng);                              // the target method's technique (the others choose per player)
        pval dv = p.at("dynamics", t, pval::of({pval::text("mf")}), rng);
        bool level = dv.is_num() || numeric_range(dv);
        std::string dyn = level ? level_dynamics(draw_in(dv, rng)) : pick_from(dv, rng).str();
        double gain = level ? level_gain(draw_in(dv, rng)) : 1;
        const std::string* dkey = strict ? &dyn : nullptr;
        double seat_secs = number_of(p.at("seat", t, pval::number(0), rng), "seat");
        double inertia = draw_in(p.at("inertia", t, pval::number(0), rng), rng);
        pval leap = p.at("leap", t, pval::none(), rng);
        pval spread = p.at("spread", t, pval::number(0), rng);
        double weight = draw_in(p.at("weight", t, pval::number(0), rng), rng);
        double tilt = draw_in(p.at("tilt", t, pval::number(0), rng), rng);
        auto weigh = [lo, hi, weight, tilt](double m) { return band_weight(lo, hi, weight, tilt, m); };
        pval chord_set = p.at("chords", t, pval::none(), rng);
        double chord_weight = draw_in(p.at("chord-weight", t, pval::number(1), rng), rng);
        pval chord;
        if (!chord_set.is_nil() && !(rand01(rng) >= chord_weight)) chord = pick_from(chord_set, rng);
        double coupling = draw_in(p.at("coupling", t, pval::number(1), rng), rng);
        // the free players: those whose last note is over (with 'continue, none of whose bookings covers now; each keeps
        // a note only until its next booking)
        std::vector<int> free;
        for (size_t q = 0; q < np; q++) {
            const player& pl = players[q];
            double f;
            if (!carry) { if (!(pl.busy_until <= T)) continue; f = 1e9; }
            else if (pl.starts.empty()) f = 1e9;
            else {
                bool covering = false; double first = 1e300;
                for (size_t j = 0; j < pl.starts.size(); j++) {
                    if (pl.starts[j] <= T && j < pl.ends.size() && pl.ends[j] > T) covering = true;
                    first = std::min(first, pl.starts[j] + 1e9 * (pl.starts[j] <= T ? 1 : 0));
                }
                if (covering) continue;
                f = first;
            }
            till[q] = f;
            if (f - T > 0.05) free.push_back((int)q);
        }
        double poly = std::round(draw_in(p.at("polyphony", t, pval::number(1000), rng), rng));
        if (due && free.empty() && is_target && steal) {        // the density is due and nobody is free: the oldest note gives way
            long oldest = -1;
            for (size_t q = 0; q < np; q++) if (players[q].busy_until > T && (oldest < 0 || since[q] < since[(size_t)oldest])) oldest = (long)q;
            if (oldest >= 0) {
                if (current[(size_t)oldest] >= 0) { note& cur = res.notes[(size_t)current[(size_t)oldest]]; cur.dur = std::max(0.05, t - cur.at); }
                players[(size_t)oldest].busy_until = T;
                free.push_back((int)oldest);
            }
        }
        double busy = (double)(np - free.size());
        // who may play the event: the styles of the set each has samples of in the register (at the dynamics when
        // strict), on the instrument it is seated at
        auto instrs_of = [&](size_t q) -> std::vector<std::string> {
            const player& pl = players[q];
            if (seat_secs > 0 && pl.seated && T < pl.seat_since + seat_secs) return {pl.seat};
            return pl.instrs;
        };
        std::map<std::string, std::vector<std::string>> memo;
        auto styles_for = [&](const std::vector<std::string>& ins) -> const std::vector<std::string>& {
            std::string key; for (size_t j = 0; j < ins.size(); j++) key += (j ? "|" : "") + ins[j];
            auto it = memo.find(key);
            if (it != memo.end()) return it->second;
            std::vector<std::string> got;
            for (auto& s : style_set.list) {
                std::string tech = s.str();
                for (auto& i : ins) if (pc.has(i, tech, dkey, lo, hi)) { got.push_back(tech); break; }
            }
            return memo[key] = got;
        };
        auto in_group = [&](size_t q) { return !has_gsel || players[q].group == gsel; };
        std::vector<int> able;
        if (is_target) able = free;
        else for (int q : free) if (in_group((size_t)q) && !styles_for(instrs_of((size_t)q)).empty()) able.push_back(q);
        if (due) res.rep.due++;
        if (due && !is_target && able.empty()) {
            bool any = false;
            for (size_t q = 0; q < np && !any; q++) if (in_group(q) && !styles_for(players[q].instrs).empty()) any = true;
            if (!any) {
                res.rep.skipped_unplayable++;
                std::string key = style_set.str() + "|" + num_text(std::round(reg.list[0].num)) + "|" + num_text(std::round(reg.list[1].num)) + "|" + (strict ? dyn : "");
                if (std::find(unplayable_keys.begin(), unplayable_keys.end(), key) == unplayable_keys.end()) {
                    unplayable_keys.push_back(key);
                    unplayable u; u.at = t; u.styles = style_set; u.reg = reg; u.strict = strict; u.dyn = dyn;
                    res.rep.unplayables.push_back(u);
                    auto fixed1 = [](double x) { return num_text(std::round(x * 10) / 10); };
                    if (unplayable_keys.size() <= 4)
                        res.messages.push_back("orchestrate-granular: no instrument of the orchestra has samples of " + style_set.str() + (strict ? " at " + dyn : "") +
                                               " in octaves " + fixed1(reg.list[0].num) + " - " + fixed1(reg.list[1].num) + " (at " + fixed1(t) + " s): unplayable, nothing written");
                    if (unplayable_keys.size() == 5) res.messages.push_back("orchestrate-granular: ... and more of the same (each style set and register once)");
                }
            } else res.rep.skipped_busy++;
        }
        if (due && !able.empty() && busy >= poly) res.rep.skipped_busy++;
        if (due && !able.empty() && busy < poly) {
            res.rep.events++;
            int pq = able[(size_t)std::floor(rand01(rng) * (double)able.size())];
            std::vector<int> group;
            if (players[(size_t)pq].group < 0 || rand01(rng) >= coupling) group.push_back(pq);
            else for (int q : able) if (players[(size_t)q].group == players[(size_t)pq].group) group.push_back(q);
            size_t room = (size_t)std::max(1.0, poly - busy);
            if (group.size() > room) group.resize(room);
            if (is_target) {
                // the target's spectrum now less what still sounds, pursued over the free players' sounds at its peaks
                std::vector<const note*> sounding;
                for (size_t q = 0; q < np; q++) if (players[q].busy_until > T && current[q] >= 0) sounding.push_back(&res.notes[(size_t)current[q]]);
                std::vector<note> held;                         // copies: the notes vector grows below
                for (auto* s : sounding) held.push_back(*s);
                std::vector<const note*> sp; for (auto& h : held) sp.push_back(&h);
                auto register_now = [&] { return p.at("register", t, pval::range(0, 8), rng); };
                auto alive = [&] { return !hooks.progress || hooks.progress(secs > 0 ? t / secs : 1); };
                for (choice& c : target_notes(ix, ec, *tgt, t, free, players, sp, register_now, dopt, rng, hooks.trace, alive)) {
                    size_t q = (size_t)c.player;
                    c.n.player = c.player;
                    double cdur = has_duration ? draw_in(p.at("duration", t, pval::range(0.1, 0.3), rng), rng) : c.life;
                    c.n.at = t; c.n.dur = cdur;
                    res.notes.push_back(c.n);
                    current[q] = (long)res.notes.size() - 1;
                    since[q] = t;
                    players[q].busy_until = T + cdur;
                    if (carry) { players[q].starts.push_back(T); players[q].ends.push_back(T + cdur); }
                    players[q].has_last_pitch = true; players[q].last_pitch = c.n.midi;
                    st_notes[q]++; st_secs[q] += cdur;
                    voice_k++;
                }
            } else {
                for (int qi : group) {
                    size_t q = (size_t)qi; player& pl = players[q];
                    const std::vector<std::string> have = styles_for(instrs_of(q));
                    std::string qtech;
                    bool keep = false;
                    if (pl.has_last_tech) {
                        if (rand01(rng) < inertia && std::find(have.begin(), have.end(), pl.last_tech) != have.end()) keep = true;
                    }
                    if (keep) qtech = pl.last_tech;
                    else qtech = have[(size_t)std::floor(rand01(rng) * (double)have.size())];
                    std::vector<std::string> ins = instrs_of(q);
                    // the pitches of the player's samples with that technique in the register
                    std::vector<double> playable;
                    for (auto& i : ins) for (int m : pc.pitches(i, &qtech, dkey)) if (m >= lo && m <= hi) playable.push_back(m);
                    std::sort(playable.begin(), playable.end()); playable.erase(std::unique(playable.begin(), playable.end()), playable.end());
                    if (!leap.is_nil() && pl.has_last_pitch) {           // within the leap of the last pitch, when there are samples there
                        std::vector<double> nearest;
                        for (double m : playable) {
                            double d = m - pl.last_pitch;
                            if (leap.is_list()) { if (leap.list.size() >= 2 && d >= number_of(leap.list.front(), "leap") && d <= number_of(leap.list.back(), "leap")) nearest.push_back(m); }
                            else if (std::fabs(d) <= number_of(leap, "leap")) nearest.push_back(m);
                        }
                        if (!nearest.empty()) playable = nearest;
                    }
                    if (playable.empty()) throw std::runtime_error("orchestrate-granular: a player without a pitch");
                    // a pitch by the method
                    std::vector<double> chord_pitches; bool has_chord = !chord.is_nil();
                    if (has_chord) { if (chord.is_list()) for (auto& c : chord.list) chord_pitches.push_back(pitch_number(c)); else chord_pitches.push_back(pitch_number(chord)); }
                    auto nearest = [&](double m) {
                        double best = playable[0], bd = std::fabs(playable[0] - m);
                        for (double x : playable) if (std::fabs(x - m) < bd) { bd = std::fabs(x - m); best = x; }
                        return best;
                    };
                    auto in_register = [&](double m) { double x = m; while (x < lo) x += 12; while (x > hi) x -= 12; return std::max(lo, std::min(hi, x)); };
                    auto pick_weighted = [&](const std::vector<double>& items) {
                        std::vector<double> ws; double total = 0;
                        for (double x : items) { double w = std::max(0.0, weigh(x)); ws.push_back(w); total += w; }
                        if (total <= 0) return items[(size_t)std::floor(rand01(rng) * (double)items.size())];
                        double u = rand01(rng) * total; size_t k = 0; double acc = ws[0];
                        while (k + 1 < items.size() && u >= acc) { k++; acc += ws[k]; }
                        return items[k];
                    };
                    auto random_pitch = [&](const std::vector<double>& pl2) {
                        if (!has_chord) return pick_weighted(pl2);
                        std::vector<double> cands;
                        for (double m : pl2) for (double c : chord_pitches) if (std::fmod(m, 12) == std::fmod(c, 12)) { cands.push_back(m); break; }
                        return pick_weighted(cands.empty() ? pl2 : cands);
                    };
                    double pitch;
                    if (method == "cluster") {
                        int least = INT_MAX;
                        for (double m : playable) least = std::min(least, covered.count(m) ? covered[m] : 0);
                        std::vector<double> fresh_ps;
                        for (double m : playable) if ((covered.count(m) ? covered[m] : 0) == least) fresh_ps.push_back(m);
                        pitch = random_pitch(fresh_ps);
                        covered[pitch] = (covered.count(pitch) ? covered[pitch] : 0) + 1;
                    } else if (method == "pivots") {
                        std::vector<double> centres = has_chord ? chord_pitches : std::vector<double>{(lo + hi) / 2};
                        double centre = in_register(centres[(size_t)std::fmod((double)voice_k, (double)centres.size())]);
                        double reach = draw_in(p.at("interval", t, pval::number(3), rng), rng);
                        std::vector<double> around;
                        for (double m : playable) if (std::fabs(m - centre) <= reach) around.push_back(m);
                        pitch = !around.empty() ? pick_weighted(around) : nearest(centre);
                    } else if (method == "markov") {
                        const std::vector<double>* row = nullptr;
                        if (pl.has_last_pitch && table) for (auto& kv : *table) if (kv.first == pl.last_pitch) { row = &kv.second; break; }
                        if (row && !row->empty()) {
                            std::vector<double> can;
                            for (double m : *row) { double x = in_register(m); if (in_list(playable, x)) can.push_back(x); }
                            if (!can.empty()) pitch = can[(size_t)std::floor(rand01(rng) * (double)can.size())];
                            else pitch = nearest(in_register((*row)[(size_t)std::floor(rand01(rng) * (double)row->size())]));
                        } else if (table && !table->empty()) {
                            pitch = nearest(in_register((*table)[(size_t)std::floor(rand01(rng) * (double)table->size())].first));
                        } else pitch = random_pitch(playable);
                    } else if (method == "harmonic") {
                        double u = rand01(rng);
                        bool partial = false;
                        if (u < draw_in(p.at("harmonicity", t, pval::number(0.7), rng), rng)) {
                            std::vector<double> partials;
                            for (double m : harmonic_series(fundamental, 16)) if (in_list(playable, m)) partials.push_back(m);
                            if (!partials.empty()) { pitch = pick_weighted(partials); partial = true; }
                        }
                        if (!partial) pitch = random_pitch(playable);
                    } else pitch = random_pitch(playable);                // random, chordinterp (the envelope interpolates the chords)
                    // the instrument: of an ossia, one recorded at that pitch with the technique
                    std::string instr;
                    if (ins.size() == 1) instr = ins[0];
                    else {
                        std::vector<std::string> with;
                        for (auto& i : ins) { const auto& ps = pc.pitches(i, &qtech, dkey); if (std::find(ps.begin(), ps.end(), (int)pitch) != ps.end() && (double)(int)pitch == pitch) with.push_back(i); }
                        if (with.empty()) throw std::runtime_error("instrument-for: no instrument of the player was recorded at " + midi_to_pitch((int)std::lround(pitch)) + " with " + qtech);
                        instr = with[(size_t)std::floor(rand01(rng) * (double)with.size())];
                    }
                    note n = make_note(ix, instr, pitch, dyn, qtech);
                    n.player = (int)q;
                    if (just && method == "harmonic") { double c; if (just_cents(fundamental, pitch, 16, c)) n.cents = c; }
                    if (ix.at(n.entry).dyn != dyn) res.rep.substituted++;
                    double off = rand01(rng) * draw_in(spread, rng);
                    double ndur = carry ? std::max(0.05, std::min(dur, till[q] - T - off)) : dur;
                    n.at = t + off; n.dur = ndur; n.gain = gain * balance_gain(balance, instr);
                    res.notes.push_back(n);
                    current[q] = (long)res.notes.size() - 1;
                    pl.busy_until = (T + off) + ndur;
                    if (carry) { pl.starts.push_back(T + off); pl.ends.push_back((T + off) + ndur); }
                    pl.has_last_pitch = true; pl.last_pitch = pitch;
                    pl.has_last_tech = true; pl.last_tech = qtech;
                    if (seat_secs > 0 && pl.instrs.size() > 1 && !(pl.seated && pl.seat == instr)) { pl.seated = true; pl.seat = instr; pl.seat_since = T; }
                    st_notes[q]++; st_secs[q] += ndur;
                    voice_k++;
                }
            }
        }
        t = t + step;
    }
    report& r = res.rep;
    r.seconds = secs; r.notes = (int)res.notes.size();
    r.saturation = (double)r.skipped_busy / std::max(1, r.due);
    for (size_t q = 0; q < np; q++) {
        player_report pr; pr.instrs = players[q].instrs; pr.notes = st_notes[q];
        pr.busy = std::min(1.0, st_secs[q] / std::max(1e-9, secs));
        if (players[q].seated) pr.seat = players[q].seat;
        r.players.push_back(pr);
    }
    if (hooks.progress) hooks.progress(1);
    return res;
}

// (orchestrate-morphological db orch target params)'s parameters for the granulator: the target's envelopes, the method,
// and those of params that are not the envelopes' own (step, width, range, min-density, density-scale, solutions)
inline params morphological_params(const target& g, const params& user) {
    envelope_options eo;
    eo.step = user.num("step", 0.25); eo.width = user.num("width", 1); eo.min_density = user.num("min-density", 0.5);
    eo.density_scale = user.num("density-scale", 1); eo.range = user.num("range", 40);
    envelopes e = target_envelopes(g, eo);
    params p;
    p.set("method", pval::text("target"));
    p.set("density", e.density); p.set("register", e.reg); p.set("dynamics", e.dynamics);
    static const char* const own[] = {"step", "width", "range", "min-density", "density-scale", "solutions"};
    for (auto& kv : user.m) {
        bool skip = false; for (const char* o : own) if (kv.first == o) skip = true;
        if (!skip) p.set(kv.first, kv.second);
    }
    return p;
}

// (markov-learn lines): the Markov method's table from melodic lines (MIDI pitches in time order, one line per
// instrument): each pitch and the pitches that followed it, repeated followers weighing more
inline markov_table markov_learn(const std::vector<std::vector<double>>& lines) {
    markov_table t;
    for (const auto& line : lines)
        for (size_t k = 0; k + 1 < line.size(); k++) {
            auto it = std::find_if(t.begin(), t.end(), [&](const std::pair<double, std::vector<double>>& r) { return r.first == line[k]; });
            if (it != t.end()) it->second.push_back(line[k + 1]);
            else t.push_back({line[k], {line[k + 1]}});
        }
    return t;
}
// a realisation's notes as an orchestration's connection makes them (music.mu's connection): in time order, a note that
// continues one of the same player, instrument and pitch merged into it (never longer than hold seconds); => the place
// of each note kept in notes and its duration
inline std::vector<std::pair<size_t, double>> connection_plan(const std::vector<note>& notes, double hold) {
    std::vector<size_t> order(notes.size());
    for (size_t k = 0; k < order.size(); k++) order[k] = k;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return notes[a].at < notes[b].at; });
    std::vector<span> spans;
    for (size_t k : order) spans.push_back({notes[k].at, notes[k].dur, notes[k].instr + "|" + num_text(notes[k].midi) + "|" + std::to_string(notes[k].player)});
    std::vector<std::pair<size_t, double>> out;
    for (auto& p : continuation_plan(spans, hold)) out.push_back({order[p.first], p.second});
    return out;
}

// (orchestrate-morphological db orch target params)'s one realisation: the granulator driven by the target over its
// length, the decision at every event the pursuit (or Orchidea's search) against the target's spectrum
inline run_result orchestrate_morphological(const dbindex& ix, std::vector<player>& orch, const target& g, const params& user, rng_t& rng,
                                            const run_hooks& hooks = {}) {
    params p = morphological_params(g, user);
    return granulate(ix, orch, g.seconds, p, &g, nullptr, rng, hooks);
}

}  // namespace granular
}  // namespace musil
