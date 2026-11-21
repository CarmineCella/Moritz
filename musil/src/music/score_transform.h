// score_transform.h — what is done to a selection of a score: transpose, invert, retrograde, augment and diminish,
// quantize, humanize, thin, density, shift. Pure functions on the C++ score (score.h), without the interpreter:
// Musil's builtins (score-transpose! ...) and Moritz's roll (its Transform buttons) and score object (its messages)
// call these, so a transformation gives the same score everywhere.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// Each function changes the score it is given, in place, and only the events whose ids are listed (none listed:
// every event of the score; ids it does not hold are ignored). The events keep their ids. Times are seconds, pitches
// MIDI (fractional for microtones), as in score.h. The span of a selection is from its first onset to its last end;
// retrograde, augment and density work within it. What is random (humanize, thin, density) draws from a generator
// seeded with the seed given, through Musil's portable random module (rng.h): the same seed, the same result on
// every system. The pitch transformations change notes (and, for transpose, the transposition of a score placed);
// a chord of Musil's (variant "chord") keeps its pitches, which only Musil holds.
#pragma once
#include "score.h"
#include "../rng.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_set>
#include <vector>

namespace musil {
namespace score {
namespace transform {

using Ids = std::vector<uint64_t>;

// the events of ids (every event when ids is empty), in the order of the score
inline std::vector<Event*> pick(Score& s, const Ids& ids) {
    std::vector<Event*> out;
    if (ids.empty()) {
        for (auto& e : s.events()) out.push_back(e.get());
        return out;
    }
    std::unordered_set<uint64_t> want(ids.begin(), ids.end());
    for (auto& e : s.events()) if (want.count(e->id)) out.push_back(e.get());
    return out;
}
inline bool pitched(const Event& e) { return e.kind == Kind::Note && e.variant != "chord"; }

// from the first onset to the last end of the events; false when there are none
inline bool span(const std::vector<Event*>& evs, double& t0, double& t1) {
    if (evs.empty()) return false;
    t0 = evs[0]->at; t1 = evs[0]->end();
    for (auto* e : evs) { t0 = std::min(t0, e->at); t1 = std::max(t1, e->end()); }
    return true;
}
// the middle of the written pitches of the notes (the axis the roll inverts around); false when there are none
inline bool pitchMiddle(Score& s, const Ids& ids, double& axis) {
    double lo = 0, hi = 0; bool any = false;
    for (auto* e : pick(s, ids)) {
        if (!pitched(*e)) continue;
        lo = any ? std::min(lo, e->pitch) : e->pitch;
        hi = any ? std::max(hi, e->pitch) : e->pitch;
        any = true;
    }
    if (any) axis = (lo + hi) / 2;
    return any;
}

// n semitones higher (lower when negative): the notes, and the scores placed (their transposition); => how many changed
inline size_t transpose(Score& s, double n, const Ids& ids = {}) {
    size_t c = 0;
    for (auto* e : pick(s, ids)) {
        if (pitched(*e)) { e->pitch += n; c++; }
        else if (e->kind == Kind::Score) { e->transpose += n; c++; }
    }
    if (c) s.touch();
    return c;
}
// the notes mirrored around a pitch: what sounded at p + c cents sounds at 2 axis - p - c (twice: as it was)
inline size_t invert(Score& s, double axis, const Ids& ids = {}) {
    size_t c = 0;
    for (auto* e : pick(s, ids)) {
        if (!pitched(*e)) continue;
        e->pitch = 2 * axis - e->pitch;
        e->cents = -e->cents;
        if (e->cents == 0) e->cents = 0;          // not -0
        c++;
    }
    if (c) s.touch();
    return c;
}
// backwards within the selection's span: an event that ended t before the span's end starts t after its start
inline size_t retrograde(Score& s, const Ids& ids = {}) {
    auto evs = pick(s, ids);
    double t0, t1;
    if (!span(evs, t0, t1)) return 0;
    for (auto* e : evs) e->at = std::max(0.0, t0 + t1 - e->end());
    s.touch();
    return evs.size();
}
// onsets and durations stretched by k (> 0) from origin (augment: k > 1; diminish: k < 1); an onset that would fall
// before 0 stays at 0
inline size_t augment(Score& s, double k, double origin, const Ids& ids = {}) {
    if (!(k > 0) || !std::isfinite(k)) throw Error("score: the factor must be > 0");
    auto evs = pick(s, ids);
    for (auto* e : evs) {
        e->at = std::max(0.0, origin + (e->at - origin) * k);
        e->dur *= k;
    }
    if (!evs.empty()) s.touch();
    return evs.size();
}
// the same, from the start of the selection's span
inline size_t augment(Score& s, double k, const Ids& ids = {}) {
    auto evs = pick(s, ids);
    double t0, t1;
    if (!span(evs, t0, t1)) return 0;
    return augment(s, k, t0, ids);
}
// onsets and ends moved toward the nearest multiple of grid seconds (strength 1: onto it, 0.5: half way); an event
// whose end would come at or before its onset ends a grid after it; an event of no duration keeps none
inline size_t quantize(Score& s, double grid, double strength = 1, const Ids& ids = {}) {
    if (!(grid > 0) || !std::isfinite(grid)) throw Error("score: the grid must be > 0");
    strength = std::max(0.0, std::min(1.0, strength));
    auto evs = pick(s, ids);
    for (auto* e : evs) {
        double qa = std::round(e->at / grid) * grid;
        double at = e->at + strength * (qa - e->at);
        if (e->dur <= 0) { e->at = std::max(0.0, at); continue; }
        double qe = std::max(std::round(e->end() / grid) * grid, qa + grid);
        double end = e->end() + strength * (qe - e->end());
        e->at = std::max(0.0, at);
        e->dur = std::max(0.0, end - e->at);
    }
    if (!evs.empty()) s.touch();
    return evs.size();
}
// each onset moved at random by up to seconds either way (never before 0), each gain by up to db decibels either way
inline size_t humanize(Score& s, double seconds, double db, uint32_t seed, const Ids& ids = {}) {
    std::mt19937 g(seed);
    auto evs = pick(s, ids);
    for (auto* e : evs) {
        double dt = rnd::uniform(g, -seconds, seconds);
        double dg = rnd::uniform(g, -db, db);
        e->at = std::max(0.0, e->at + dt);
        e->gain *= std::pow(10.0, dg / 20);
    }
    if (!evs.empty()) s.touch();
    return evs.size();
}
// each event kept with probability keep (as music.mu's fragment-thin); => the ids of those taken out
inline Ids thin(Score& s, double keep, uint32_t seed, const Ids& ids = {}) {
    std::mt19937 g(seed);
    Ids gone;
    for (auto* e : pick(s, ids)) if (!(rnd::uniform01(g) < keep)) gone.push_back(e->id);
    for (uint64_t id : gone) s.remove(id);
    return gone;
}
// each event kept with the probability a curve (values 0 to 1, read along the selection's span) has at its onset (as
// music.mu's fragment-density); => the ids of those taken out
inline Ids density(Score& s, const std::vector<double>& curve, uint32_t seed, const Ids& ids = {}) {
    if (curve.empty()) throw Error("score: density wants a curve (values from 0 to 1)");
    auto evs = pick(s, ids);
    double t0, t1;
    if (!span(evs, t0, t1)) return {};
    double d = std::max(1e-9, t1 - t0);
    std::mt19937 g(seed);
    Ids gone;
    size_t n = curve.size();
    for (auto* e : evs) {
        size_t k = (size_t)std::min<double>((double)n - 1, std::floor((double)n * (e->at - t0) / d));
        if (!(rnd::uniform01(g) < curve[k])) gone.push_back(e->id);
    }
    for (uint64_t id : gone) s.remove(id);
    return gone;
}
// every onset moved by dt seconds (never before 0)
inline size_t shift(Score& s, double dt, const Ids& ids = {}) {
    auto evs = pick(s, ids);
    for (auto* e : evs) e->at = std::max(0.0, e->at + dt);
    if (!evs.empty()) s.touch();
    return evs.size();
}

// --- scores inside scores ---------------------------------------------------------------------------------------------
// A copy of root in which the score old, wherever it is placed (at any depth, every instance), is now: the scores on
// the way down to it are copied (copy on write), the others shared. nullptr when root does not place old.
inline std::shared_ptr<Score> replaceInstance(const Score& root, const Score* old, const std::shared_ptr<Score>& now, int depth = 0) {
    if (depth > kMaxDepth) return nullptr;
    std::shared_ptr<Score> copy;
    for (size_t k = 0; k < root.events().size(); k++) {
        const Event& e = *root.events()[k];
        if (e.kind != Kind::Score || !e.sub) continue;
        std::shared_ptr<Score> sub;
        if (e.sub.get() == old) sub = now;
        else sub = replaceInstance(*e.sub, old, now, depth + 1);
        if (!sub) continue;
        if (!copy) copy = root.clone();
        copy->events()[k]->sub = sub;
    }
    if (copy) copy->touch();
    return copy;
}
// the score placed by the events of a path of ids, from root down (an empty path: root); nullptr when one is missing
inline std::shared_ptr<Score> descend(const std::shared_ptr<Score>& root, const std::vector<uint64_t>& path) {
    std::shared_ptr<Score> s = root;
    for (uint64_t id : path) {
        if (!s) return nullptr;
        auto e = s->find(id);
        if (!e || e->kind != Kind::Score || !e->sub) return nullptr;
        s = e->sub;
    }
    return s;
}

}  // namespace transform
}  // namespace score
}  // namespace musil
