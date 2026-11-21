// score_solutions.h — the solutions of an orchestration kept in the score it made, and another one chosen.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// An orchestration cuts a target into segments and finds for each several solutions, ranked by cost; its score holds
// the connection, one solution a segment, the notes held across a boundary merged into longer ones (continuation.h).
// The score may keep every segment's solutions, so that another can be chosen later (Moritz's roll and its orchidea
// object's choose, Musil's score-choose!): this file is the one implementation of that choice.
//
// Where they are. In the score's extra, under "solutions" (written to .score files as any other data, so a score
// without it reads and behaves exactly as before, and one with it survives saving, undo, copies and placements):
//   (list ORCH ...)                        one an orchestration, in the order they were made
//   ORCH  (list at hold SEG ...)           at: where its times start in the score (seconds); hold: the longest a held
//                                          note may last (seconds; 1e9: no limit)
//   SEG   (list at dur chosen SOL ...)     its time (from the orchestration's at), the rank chosen (from 0), the
//                                          solutions best first
//   SOL   (list cost NOTE ...)             a solution: its cost, its notes (a rest has none)
//   NOTE  (list row midi cents dyn tech az gain sound player)
//                                          an instrument's note over the whole segment: the row, the sample's MIDI
//                                          pitch (-1: none, the note at 60), its cents, dynamics, technique, place, gain,
//                                          the sample's path ("" none) and the player (an index of the orchestra)
// Each note the connection placed carries in its extra "orchestration", the index of its orchestration, and as an
// orchestra object's notes do, "sound", "sound-pitch" and "player".
//
// Choosing. choose() puts solution `rank` of a segment in place of the one chosen: the orchestration's connection is
// made again with that choice (connection(): the chosen notes of every segment, the held ones merged by the same rule
// as before), and in the score only the notes around the segment are replaced: those of the orchestration that sound
// during the segment are taken out, and the new connection's notes that sound then are put in; a note held across a
// boundary widens that window (into the neighbour, until no note crosses its edges), so a note held from the segment
// before into this one is cut back, or lengthened, to what the new choice holds, and the neighbours' other notes
// (and whatever the user changed there, and every note not of the orchestration) are kept. On a score holding the
// connection it was made with, the result is the connection of the new choices, note for note.
#pragma once
#include "score.h"
#include "continuation.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace musil {
namespace score {
namespace solutions {

constexpr const char* kKey = "solutions";          // the score's extra
constexpr const char* kTag = "orchestration";      // a note's extra: the index of the orchestration it belongs to

struct Note {
    std::string row;
    double midi = -1, cents = 0;
    std::string dyn, tech;
    double az = 0, gain = 1;
    std::string sound;
    double player = 0;
};
struct Solution {
    double cost = 0;
    std::vector<Note> notes;
};
struct Segment {
    double at = 0, dur = 0;
    int chosen = 0;
    std::vector<Solution> solutions;
};
struct Orchestration {
    double at = 0, hold = 1e9;
    std::vector<Segment> segments;
};

// ---- reading and writing (a malformed entry of a file read is an Error naming it; a score without any: none)
namespace detail {
inline double num(const Datum& d, const char* what) {
    if (d.kind != Datum::Num || !std::isfinite(d.num)) throw Error(std::string("score: solutions: ") + what + " is not a number");
    return d.num;
}
inline std::string word(const Datum& d, const char* what) {
    if (d.kind == Datum::Str || d.kind == Datum::Sym) return d.s;
    if (d.kind == Datum::Num) return numText(d.num);
    throw Error(std::string("score: solutions: ") + what + " is not a word");
}
inline const std::vector<Datum>& list(const Datum& d, size_t least, const char* what) {
    if (d.kind != Datum::List || d.list.size() < least) throw Error(std::string("score: solutions: ") + what + " is not a list as it should be");
    return d.list;
}
}  // namespace detail

inline Orchestration orchestrationOf(const Datum& d) {
    using namespace detail;
    const auto& o = list(d, 2, "an orchestration");
    Orchestration r;
    r.at = std::max(0.0, num(o[0], "an orchestration's at"));
    r.hold = std::max(0.0, num(o[1], "an orchestration's hold"));
    for (size_t k = 2; k < o.size(); k++) {
        const auto& g = list(o[k], 3, "a segment");
        Segment s;
        s.at = num(g[0], "a segment's at");
        s.dur = num(g[1], "a segment's duration");
        if (s.at < 0 || s.dur < 0) throw Error("score: solutions: a segment with a negative time");
        double c = num(g[2], "a segment's choice");
        for (size_t j = 3; j < g.size(); j++) {
            const auto& x = list(g[j], 1, "a solution");
            Solution sol;
            sol.cost = num(x[0], "a solution's cost");
            for (size_t n = 1; n < x.size(); n++) {
                const auto& v = list(x[n], 9, "a note of a solution");
                Note nt;
                nt.row = word(v[0], "a note's row");
                nt.midi = num(v[1], "a note's pitch");
                nt.cents = num(v[2], "a note's cents");
                nt.dyn = word(v[3], "a note's dynamics");
                nt.tech = word(v[4], "a note's technique");
                nt.az = num(v[5], "a note's azimuth");
                nt.gain = num(v[6], "a note's gain");
                nt.sound = word(v[7], "a note's sound");
                nt.player = num(v[8], "a note's player");
                sol.notes.push_back(std::move(nt));
            }
            s.solutions.push_back(std::move(sol));
        }
        s.chosen = s.solutions.empty() ? 0 : (int)std::max(0.0, std::min(c, (double)s.solutions.size() - 1));
        r.segments.push_back(std::move(s));
    }
    return r;
}
inline Datum datumOf(const Orchestration& o) {
    std::vector<Datum> out{Datum::number(o.at), Datum::number(o.hold)};
    for (auto& s : o.segments) {
        std::vector<Datum> g{Datum::number(s.at), Datum::number(s.dur), Datum::number(s.chosen)};
        for (auto& sol : s.solutions) {
            std::vector<Datum> x{Datum::number(sol.cost)};
            for (auto& n : sol.notes)
                x.push_back(Datum::of({Datum::symbol(n.row), Datum::number(n.midi), Datum::number(n.cents), Datum::symbol(n.dyn),
                                       Datum::symbol(n.tech), Datum::number(n.az), Datum::number(n.gain), Datum::string(n.sound),
                                       Datum::number(n.player)}));
            g.push_back(Datum::of(std::move(x)));
        }
        out.push_back(Datum::of(std::move(g)));
    }
    return Datum::of(std::move(out));
}
// the orchestrations a score keeps (none: an empty list); an Error when what it keeps does not read
inline std::vector<Orchestration> read(const Score& s) {
    std::vector<Orchestration> out;
    const Datum* d = field(s.extra, kKey);
    if (!d) return out;
    for (auto& o : detail::list(*d, 0, "the solutions")) out.push_back(orchestrationOf(o));
    return out;
}
// the same, never an Error: what does not read is left out (what a view draws)
inline std::vector<Orchestration> readOrNone(const Score& s) {
    try { return read(s); } catch (const Error&) { return {}; }
}
inline void write(Score& s, const std::vector<Orchestration>& os) {
    if (os.empty()) { eraseField(s.extra, kKey); return; }
    std::vector<Datum> l;
    for (auto& o : os) l.push_back(datumOf(o));
    setField(s.extra, kKey, Datum::of(std::move(l)));
}

// ---- the connection: the chosen solution of every segment, the held notes merged; the events (ids 0, at the score's
// times) as the score holds them, each tagged with the orchestration's index
inline std::shared_ptr<Event> eventOf(const Note& n, double at, double dur, size_t index) {
    auto e = std::make_shared<Event>();
    e->kind = Kind::Note;
    e->at = at;
    e->dur = dur;
    e->row = n.row;
    e->pitch = n.midi >= 0 ? n.midi : 60;
    e->cents = n.cents;
    e->dyn = n.dyn;
    e->tech = n.tech;
    e->az = n.az;
    e->gain = n.gain;
    if (!n.sound.empty()) {
        setField(e->extra, "sound", Datum::string(n.sound));
        setField(e->extra, "sound-pitch", Datum::number(e->pitch));
    }
    setField(e->extra, "player", Datum::number(n.player));
    setField(e->extra, kTag, Datum::number((double)index));
    return e;
}
inline std::vector<std::shared_ptr<Event>> connection(const Orchestration& o, size_t index) {
    struct One { const Note* n; double at, dur; };
    std::vector<One> all;
    for (auto& s : o.segments) {
        if (s.solutions.empty()) continue;
        const Solution& sol = s.solutions[(size_t)std::max(0, std::min(s.chosen, (int)s.solutions.size() - 1))];
        for (auto& n : sol.notes) all.push_back({&n, o.at + s.at, s.dur});
    }
    std::stable_sort(all.begin(), all.end(), [](const One& a, const One& b) { return a.at < b.at; });
    std::vector<span> spans;
    spans.reserve(all.size());
    for (auto& x : all) spans.push_back({x.at, x.dur, x.n->row + "|" + numText(x.n->midi) + "|" + numText(x.n->player)});
    std::vector<std::shared_ptr<Event>> out;
    for (auto& p : continuation_plan(spans, o.hold)) out.push_back(eventOf(*all[p.first].n, all[p.first].at, p.second, index));
    return out;
}
// is this event a note of orchestration index?
inline bool tagged(const Event& e, size_t index) {
    const Datum* d = field(e.extra, kTag);
    return d && d->kind == Datum::Num && d->num == (double)index;
}

// ---- choosing
struct Choice {
    bool changed = false;
    std::vector<uint64_t> removed, added;          // the ids of the notes taken out and of those put in
    double from = 0, to = 0;                       // the window replaced (seconds of the score)
};
// solution rank of segment seg of orchestration orch (all from 0) in place of the one chosen; an Error when there is no
// such orchestration, segment or solution, or what the score keeps does not read. Nothing changes when the score
// already holds what the choice makes (changed false).
inline Choice choose(Score& s, size_t orch, size_t seg, size_t rank) {
    auto os = read(s);
    if (orch >= os.size()) throw Error("score: no orchestration " + std::to_string(orch + 1) + " in the score (it keeps " + std::to_string(os.size()) + ")");
    Orchestration& o = os[orch];
    if (seg >= o.segments.size()) throw Error("score: the orchestration has no segment " + std::to_string(seg + 1) + " (it has " + std::to_string(o.segments.size()) + ")");
    Segment& g = o.segments[seg];
    if (rank >= g.solutions.size()) throw Error("score: segment " + std::to_string(seg + 1) + " has " + std::to_string(g.solutions.size()) + " solutions");
    int was = g.chosen;
    g.chosen = (int)rank;
    auto fresh = connection(o, orch);
    std::vector<std::shared_ptr<Event>> old;
    for (auto& e : s.events())
        if (e->kind == Kind::Note && tagged(*e, orch)) old.push_back(e);
    // the window: the segment, widened by every note (old or new) that sounds in it, until none crosses its edges
    Choice c;
    c.from = o.at + g.at;
    c.to = c.from + g.dur;
    const double eps = 1e-9;
    auto inside = [&](const Event& e) {
        if (e.dur <= 0) return e.at >= c.from - eps && e.at < c.to - eps;
        return e.at < c.to - eps && e.end() > c.from + eps;
    };
    for (bool grew = true; grew;) {
        grew = false;
        for (auto* set : {&old, &fresh})
            for (auto& e : *set)
                if (inside(*e) && (e->at < c.from - eps || e->end() > c.to + eps)) {
                    c.from = std::min(c.from, e->at);
                    c.to = std::max(c.to, e->end());
                    grew = true;
                }
    }
    std::vector<std::shared_ptr<Event>> out, in;
    for (auto& e : old) if (inside(*e)) out.push_back(e);
    for (auto& e : fresh) if (inside(*e)) in.push_back(e);
    // the same notes already there: nothing to do (but the choice, which the score keeps)
    auto key = [](const Event& e) {
        const Datum* snd = field(e.extra, "sound");
        const Datum* pl = field(e.extra, "player");
        return std::make_tuple(e.at, e.row, e.pitch, e.dur, e.cents, e.dyn, e.tech, e.az, e.gain, snd ? snd->s : std::string(),
                               pl && pl->kind == Datum::Num ? pl->num : -1.0);
    };
    auto sorted = [&](const std::vector<std::shared_ptr<Event>>& v) {
        std::vector<decltype(key(*v.front()))> k;
        for (auto& e : v) k.push_back(key(*e));
        std::sort(k.begin(), k.end());
        return k;
    };
    bool same = out.size() == in.size() && (out.empty() || sorted(out) == sorted(in));
    if (!same) {
        for (auto& e : out) { c.removed.push_back(e->id); s.remove(e->id); }
        for (auto& e : in) c.added.push_back(s.insert(e));
    }
    if (was != (int)rank || !same) {
        write(s, os);
        s.touch();
        c.changed = true;
    }
    return c;
}

// the segment of an orchestration at a time of the score (-1: none)
inline int segmentAt(const Orchestration& o, double t) {
    for (size_t k = 0; k < o.segments.size(); k++) {
        double a = o.at + o.segments[k].at;
        if (t >= a && t < a + o.segments[k].dur) return (int)k;
    }
    return -1;
}

}  // namespace solutions
}  // namespace score
}  // namespace musil
