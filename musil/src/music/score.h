// score.h — the score as one C++ structure: things placed in time.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// A score holds events in time, rows (tracks, or the instruments of the notes), a tempo and a meter. It
// depends neither on Musil's interpreter nor on Moritz: Musil shows it to the language as a value (music.mu
// works on it, score_musil.h), Moritz includes it to play and edit scores, and the readers of .score files
// (score_text.h) and the generators make it.
//
// Events. Every event has an onset and a duration in seconds (at, dur), a gain (linear), a direction
// (azimuth and elevation in degrees, 0 in front, azimuth positive to the left), a row and a stable id: ids
// are never reused in a score, even after an event is removed, so that a view, an undo or Musil can name an
// event. What it is depends on its kind:
//   note     a pitch (MIDI, fractional for microtones) and a deviation in cents, dynamics, technique, played
//            by the instrument of its row (Musil: a sound from a database; Moritz: whatever the notes outlet feeds)
//   sound    a region (from, to in seconds) of a file (source is its path) or of a named buffer (buffer)
//   patch    a process that runs while the event lasts: source names it, values are its arguments
//            (Musil keeps its synths and calls here; Moritz will run patches)
//   score    another score placed inside this one (sub): by reference (ref is the file it comes from) or
//            embedded; a placement stretches its time, transposes its notes, scales its gain and turns its
//            directions. A score cannot contain itself, directly or through others: insert() refuses it.
//   message  a name (source) and values: an automation point, a message to a receive name
// The fields a kind does not use are left at their defaults. Anything else an event or a score carries is
// in extra (persisted: a number, a string, a symbol or a list of them) or in host (a host's own payload:
// Musil keeps the functions, vectors and databases of its events there; never written, never shared
// between hosts).
//
// Time. Seconds, exactly as written. A beat is 1/den of a bar of num beats, at bpm beats a minute
// (beats(), bars()), as in Musil's notation. Placements compose: a note at time t inside a score placed at
// T with stretch k sounds at T + k t, transposed by the sum of the transpositions, with the gains multiplied
// and the azimuths added; it is cut to its placement's window (flatten()).
//
// Editing and queries. Events are kept in the order they were added (Musil's score-events) with an index
// by id; a time index (sorted onsets and the longest duration) is made again lazily after a change, so a
// query of a time range is a binary search. insert(), remove() and replace() return what an undo needs.
// A Score is used by one thread at a time; Moritz plays copies (clone()) or plans made from them.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace musil {
namespace score {

struct Error : std::runtime_error {
    explicit Error(const std::string& m) : std::runtime_error(m) {}
};

// how deep scores may be nested (placements inside placements), as Musil's score-render
constexpr int kMaxDepth = 16;

// --- data: what a score keeps for itself ----------------------------------------------------------------------
struct Datum {
    enum Kind : uint8_t { Num, Str, Sym, List } kind = Num;
    double num = 0;
    std::string s;
    std::vector<Datum> list;
    static Datum number(double v) { Datum d; d.kind = Num; d.num = v; return d; }
    static Datum string(std::string v) { Datum d; d.kind = Str; d.s = std::move(v); return d; }
    static Datum symbol(std::string v) { Datum d; d.kind = Sym; d.s = std::move(v); return d; }
    static Datum of(std::vector<Datum> v) { Datum d; d.kind = List; d.list = std::move(v); return d; }
    bool operator==(const Datum& o) const {
        if (kind != o.kind) return false;
        if (kind == Num) return num == o.num || (std::isnan(num) && std::isnan(o.num));
        if (kind == List) return list == o.list;
        return s == o.s;
    }
    bool operator!=(const Datum& o) const { return !(*this == o); }
};
// named values in the order they were set
using Fields = std::vector<std::pair<std::string, Datum>>;
inline const Datum* field(const Fields& f, const std::string& key) {
    for (auto& kv : f) if (kv.first == key) return &kv.second;
    return nullptr;
}
inline void setField(Fields& f, const std::string& key, Datum v) {
    for (auto& kv : f) if (kv.first == key) { kv.second = std::move(v); return; }
    f.emplace_back(key, std::move(v));
}
inline bool eraseField(Fields& f, const std::string& key) {
    for (size_t k = 0; k < f.size(); k++) if (f[k].first == key) { f.erase(f.begin() + (long)k); return true; }
    return false;
}

// --- events -------------------------------------------------------------------------------------------------
enum class Kind : uint8_t { Note, Sound, Patch, Score, Message };
inline const char* kindName(Kind k) {
    switch (k) { case Kind::Note: return "note"; case Kind::Sound: return "sound"; case Kind::Patch: return "patch";
                 case Kind::Score: return "score"; case Kind::Message: return "msg"; }
    return "?";
}

struct Score;

struct Event {
    uint64_t id = 0;
    Kind kind = Kind::Note;
    double at = 0, dur = 0;          // seconds
    double gain = 1;                 // linear
    double az = 0, el = 0;           // degrees
    std::string row;                 // the row: the instrument of a note, the track of anything
    // note
    double pitch = 60;               // MIDI; fractional for microtones
    double cents = 0;                // a deviation from the pitch (sounds at pitch + cents / 100)
    std::string dyn, tech;
    // sound: a file's path or a buffer's name; patch: what runs; message: the name it is sent to
    std::string source;
    bool buffer = false;             // sound: source names a buffer, not a file
    double from = 0, to = -1;        // sound: the region in the source, seconds (to < 0: as long as the event)
    std::vector<Datum> values;       // patch: its arguments; message: its values
    // score
    std::shared_ptr<Score> sub;      // the score placed (an instance: shared with every other placement of it)
    std::string ref;                 // the file it was read from, as written ("": embedded)
    double stretch = 1, transpose = 0;
    // everything
    std::string variant;             // how a host makes it: Musil's chord, synth, call, buffer; "" otherwise
    std::string label;               // what a view shows ("": made from the fields)
    Fields extra;                    // persisted
    std::shared_ptr<void> host;      // a host's own payload: never written
    // the score holding it (kept by Score; a copy of an event is in none): a handle that changes an event in
    // place tells its score (Score::touch)
    struct Owner {
        Score* p = nullptr;
        Owner() = default;
        Owner(const Owner&) {}
        Owner& operator=(const Owner&) { return *this; }
    } owner;
    double end() const { return at + dur; }
    double sounding() const { return pitch + cents / 100; }   // the pitch heard, in MIDI
};

// --- the score ------------------------------------------------------------------------------------------------
struct Row {
    std::string name, clef;          // clef: treble, bass, alto, none or ""
    Fields extra;
};

struct Score {
    std::string name;
    double sr = 44100;
    double bpm = 60;                 // beats (1/den) a minute
    int meterNum = 4, meterDen = 4;
    std::vector<Row> rows;
    Fields extra;                    // persisted: Musil's layout, reverb...
    std::shared_ptr<void> host;      // a host's own data: never written
    std::string path;                // the file it was read from or written to ("": none)

    Score() = default;
    Score(std::string n, double rate) : name(std::move(n)), sr(rate) {}
    Score(const Score&) = delete;                 // events are owned: copies are explicit (clone)
    Score& operator=(const Score&) = delete;
    ~Score() { for (auto& e : events_) if (e->owner.p == this) e->owner.p = nullptr; }

    // a copy: its events copied (same ids), the scores placed in it shared (instances)
    std::shared_ptr<Score> clone() const {
        auto c = std::make_shared<Score>(name, sr);
        c->bpm = bpm; c->meterNum = meterNum; c->meterDen = meterDen; c->rows = rows; c->extra = extra;
        c->host = host; c->path = path; c->nextId_ = nextId_;
        c->events_.reserve(events_.size());
        for (auto& e : events_) c->adopt(std::make_shared<Event>(*e));
        return c;
    }

    // ---- the events, in the order they were added
    const std::vector<std::shared_ptr<Event>>& events() const { return events_; }
    size_t size() const { return events_.size(); }
    bool empty() const { return events_.empty(); }
    uint64_t version() const { return version_; }   // changes with every edit
    uint64_t nextId() const { return nextId_; }
    void setNextId(uint64_t n) { nextId_ = std::max<uint64_t>(n, 1); touch(); }
    uint64_t takeId() { uint64_t n = nextId_++; touch(); return n; }   // a fresh id, never used again

    std::shared_ptr<Event> find(uint64_t id) const {
        auto it = byId_.find(id);
        return it == byId_.end() ? nullptr : events_[it->second];
    }
    bool holds(const Event* e) const { auto it = byId_.find(e ? e->id : 0); return it != byId_.end() && events_[it->second].get() == e; }

    // an event added at the end (the event object itself: the caller may keep it); its id is kept unless 0 or
    // taken. A score event whose score contains this one is refused. => the id
    uint64_t insert(std::shared_ptr<Event> e) { return insertAt(events_.size(), std::move(e)); }
    uint64_t insertAt(size_t index, std::shared_ptr<Event> e) {
        if (!e) throw Error("score: no event");
        check(*e);
        if (e->id == 0 || byId_.count(e->id)) e->id = nextId_;
        nextId_ = std::max(nextId_, e->id + 1);
        index = std::min(index, events_.size());
        if (e->owner.p && e->owner.p != this) throw Error("score: the event is in another score (copy it)");
        e->owner.p = this;
        if (index == events_.size()) {               // at the end: the index of the others does not change
            byId_[e->id] = events_.size();
            events_.push_back(e);
            touch();
        } else {
            events_.insert(events_.begin() + (long)index, e);
            reindex();
        }
        return e->id;
    }
    // the event of an id taken out (nullptr when there is none); => it, with its index for an undo
    std::shared_ptr<Event> remove(uint64_t id, size_t* index = nullptr) {
        auto it = byId_.find(id);
        if (it == byId_.end()) return nullptr;
        size_t k = it->second;
        auto e = events_[k];
        events_.erase(events_.begin() + (long)k);
        if (e->owner.p == this) e->owner.p = nullptr;
        if (index) *index = k;
        reindex();
        return e;
    }
    // the event of the same id given new fields; => the old fields (nullptr when there is no such event)
    std::shared_ptr<Event> replace(const Event& now) {
        auto cur = find(now.id);
        if (!cur) return nullptr;
        check(now);
        auto old = std::make_shared<Event>(*cur);
        *cur = now;
        touch();
        return old;
    }
    // every event at once, in this order (the ids kept unless 0 or repeated)
    void assign(std::vector<std::shared_ptr<Event>> evs) {
        for (auto& e : evs) if (!e) throw Error("score: no event");
        for (auto& e : evs) check(*e);
        for (auto& e : evs) if (e->owner.p && e->owner.p != this) throw Error("score: an event is in another score (copy it)");
        for (auto& e : events_) if (e->owner.p == this) e->owner.p = nullptr;
        events_.clear(); byId_.clear();
        for (auto& e : evs) {
            if (e->owner.p == this) e = std::make_shared<Event>(*e);   // the same event twice: the second is a copy
            e->owner.p = this;
            if (e->id == 0 || byId_.count(e->id)) e->id = nextId_;
            nextId_ = std::max(nextId_, e->id + 1);
            byId_[e->id] = events_.size();
            events_.push_back(e);
        }
        touch();
    }
    void clear() { assign({}); }
    // an event changed in place (its time, its fields): the time index is made again when next needed
    void touch() { ++version_; timeDirty_ = true; }

    // ---- time
    // when the last event ends (0 when empty)
    double duration() const {
        double d = 0; bool any = false;
        for (auto& e : events_) { d = any ? std::max(d, e->end()) : e->end(); any = true; }
        return any ? d : 0;
    }
    // every event sounding in [t0, t1): onset before t1 and end after t0 (an event of no duration: its onset
    // in [t0, t1)), in onset order
    void query(double t0, double t1, const std::function<void(const Event&)>& f) const {
        index();
        auto lo = std::lower_bound(byTime_.begin(), byTime_.end(), t0 - maxDur_, [this](uint32_t k, double t) { return events_[k]->at < t; });
        for (auto it = lo; it != byTime_.end() && events_[*it]->at < t1; ++it) {
            const Event& e = *events_[*it];
            if (e.end() > t0 || (e.dur <= 0 && e.at >= t0)) f(e);
        }
    }
    // the events by onset (equal onsets in the order they were added)
    std::vector<const Event*> byTime() const {
        index();
        std::vector<const Event*> out; out.reserve(byTime_.size());
        for (uint32_t k : byTime_) out.push_back(events_[k].get());
        return out;
    }
    // seconds as beats and back, at the score's tempo
    double beats(double sec) const { return sec * bpm / 60; }
    double seconds(double beats) const { return beats * 60 / bpm; }

    // ---- placements
    // does this score contain s, directly or through the scores placed in it (or is it s)?
    bool contains(const Score* s) const {
        std::unordered_set<const Score*> seen;
        return reaches(this, s, seen, 0);
    }

private:
    std::vector<std::shared_ptr<Event>> events_;
    std::unordered_map<uint64_t, size_t> byId_;
    uint64_t nextId_ = 1, version_ = 0;
    mutable std::vector<uint32_t> byTime_;
    mutable double maxDur_ = 0;
    mutable bool timeDirty_ = true;

    void adopt(std::shared_ptr<Event> e) {
        e->owner.p = this;
        byId_[e->id] = events_.size();
        events_.push_back(std::move(e));
    }
    void reindex() {
        byId_.clear();
        for (size_t k = 0; k < events_.size(); k++) byId_[events_[k]->id] = k;
        touch();
    }
    void check(const Event& e) const {
        if (!std::isfinite(e.at) || !std::isfinite(e.dur) || e.dur < 0) throw Error("score: an event needs a finite onset and a duration >= 0");
        if (e.kind == Kind::Score) {
            if (!e.sub) throw Error("score: a score event without its score");
            if (e.sub.get() == this) throw Error("score: a score cannot contain itself");
            if (e.sub->contains(this)) throw Error("score: a score cannot contain itself (" + e.sub->name + " already contains " + name + ")");
        }
    }
    static bool reaches(const Score* from, const Score* s, std::unordered_set<const Score*>& seen, int depth) {
        if (from == s) return true;
        if (depth > 4 * kMaxDepth || !seen.insert(from).second) return false;
        for (auto& e : from->events_)
            if (e->kind == Kind::Score && e->sub && reaches(e->sub.get(), s, seen, depth + 1)) return true;
        return false;
    }
    void index() const {
        if (!timeDirty_) return;
        byTime_.resize(events_.size());
        for (uint32_t k = 0; k < (uint32_t)events_.size(); k++) byTime_[k] = k;
        std::stable_sort(byTime_.begin(), byTime_.end(), [this](uint32_t a, uint32_t b) { return events_[a]->at < events_[b]->at; });
        maxDur_ = 0;
        for (auto& e : events_) maxDur_ = std::max(maxDur_, e->dur);
        timeDirty_ = false;
    }
};

// --- placements: the scores inside brought to the top --------------------------------------------------------------
// One event of a flattened score: a copy at absolute time, gain and direction, with the transposition its
// placements add (already in pitch for a note; a host that transposes in its own way, as Musil's notes
// shifted by resampling, reads it) and how deep it was.
struct Placed {
    Event ev;
    double transpose = 0;
    int depth = 0;
    const Score* origin = nullptr;   // the score it comes from (its file's folder is where its sources are)
};
// the events of s with those of the scores inside at their place, in the order of s; a score placed is cut to
// its event's window (an inner event starting at or after the window is left out, one ending after it is
// shortened). Deeper than kMaxDepth: an Error.
inline void flatten(const Score& s, std::vector<Placed>& out, int depth = 0) {
    if (depth > kMaxDepth) throw Error("score: scores nested more than " + std::to_string(kMaxDepth) + " deep");
    for (auto& ep : s.events()) {
        const Event& e = *ep;
        if (e.kind != Kind::Score) { out.push_back({e, 0, depth, &s}); continue; }
        if (!e.sub) continue;
        std::vector<Placed> inner;
        flatten(*e.sub, inner, depth + 1);
        for (auto& p : inner) {
            double rel = p.ev.at * e.stretch;
            if (!(rel < e.dur)) continue;
            Placed q = std::move(p);
            q.ev.at = e.at + rel;
            q.ev.dur = std::min(q.ev.dur * e.stretch, e.dur - rel);
            q.ev.gain *= e.gain;
            q.ev.az += e.az;
            q.ev.el = std::max(-90.0, std::min(90.0, q.ev.el + e.el));
            if (q.ev.kind == Kind::Note) q.ev.pitch += e.transpose;
            q.transpose += e.transpose;
            out.push_back(std::move(q));
        }
    }
}

// --- bars and beats -----------------------------------------------------------------------------------------------
// A position as bars, beats (both from 1) and ticks (kTicks a beat), from seconds at a tempo and meter.
constexpr int kTicks = 480;
struct BBT { long bar = 1; int beat = 1; int tick = 0; };
inline BBT toBBT(double sec, double bpm, int num) {
    BBT p;
    if (!(bpm > 0) || num < 1 || !std::isfinite(sec)) return p;
    double beats = sec * bpm / 60;
    double whole = std::floor(beats + 1e-9);
    long b = (long)whole;
    long bar = b >= 0 ? b / num : -((-b + num - 1) / num);
    p.bar = bar + 1;
    p.beat = (int)(b - bar * num) + 1;
    p.tick = std::max(0, std::min(kTicks - 1, (int)std::floor((beats - whole) * kTicks + 1e-6)));
    return p;
}
inline double fromBBT(double bar, double beat, double tick, double bpm, int num) {
    if (!(bpm > 0) || num < 1) return 0;
    return ((bar - 1) * num + (beat - 1) + tick / kTicks) * 60 / bpm;
}

// --- where a sound goes: the gains of a mono sound at an azimuth on n channels ----------------------------------------
// 1 channel: all of it; 2: equal power between left and right (Musil's pan-azimuth: -90 right .. 90 left, behind folded
// onto the front); more: a ring of n speakers evenly around from the front going left (speaker-ring), amplitude panned
// between the two nearest (pan-n). Musil's music.mu places its sounds with these gains, Moritz's score object too.
inline void panGains(double az, int n, double* g) {
    if (n <= 0) return;
    if (n == 1) { g[0] = 1; return; }
    if (!std::isfinite(az)) az = 0;
    if (n == 2) {
        double a = -std::max(-90.0, std::min(90.0, az > 90 ? 180 - az : az < -90 ? -180 - az : az));
        double p = std::max(-1.0, std::min(1.0, a / 90));
        g[0] = std::sqrt(0.5 * (1 - p)); g[1] = std::sqrt(0.5 * (1 + p));
        return;
    }
    auto princ = [](double a) { return a - 360 * std::round(a / 360); };
    auto dist = [&](int k) { double s = k * (360.0 / n); s -= 360 * std::floor(s / 180.0001); return std::fabs(princ(az - s)); };
    int i1 = 0, i2 = -1; double d1 = 1e300, d2 = 1e300;        // no allocation: Moritz's audio side may call it
    for (int k = 0; k < n; k++) { double d = dist(k); if (d < d1) { d1 = d; i1 = k; } }
    for (int k = 0; k < n; k++) { double d = dist(k); if (k != i1 && d < d2) { d2 = d; i2 = k; } }
    double g1 = d1 + d2 < 1e-9 ? 1 : d2 / (d1 + d2), g2 = 1 - g1, norm = std::sqrt(g1 * g1 + g2 * g2);
    for (int k = 0; k < n; k++) g[k] = 0;
    g[i1] = g1 / norm;
    if (i2 >= 0) g[i2] = g2 / norm;
}

// --- dynamics as MIDI velocity: ppp 16 ... fff 127 (mf 80 when there is none) ---------------------------------------
inline int velocityOf(const std::string& dyn) {
    static const char* names[] = {"ppp", "pp", "p", "mp", "mf", "f", "ff", "fff"};
    static const int vel[] = {16, 32, 48, 64, 80, 96, 112, 127};
    for (int k = 0; k < 8; k++) if (dyn == names[k]) return vel[k];
    if (dyn == "pppp") return 8;
    if (dyn == "ffff") return 127;
    return 80;
}

// --- pitch names: C4 = 60, sharps # or _, flats b, double sharp x; -1 when not a pitch ---------------------------------
inline double pitchOfName(const std::string& s) {
    if (s.size() < 2) return -1;
    static const int base[] = {9, 11, 0, 2, 4, 5, 7};   // A B C D E F G
    char c = (char)std::toupper((unsigned char)s[0]);
    if (c < 'A' || c > 'G') return -1;
    int semi = base[c - 'A']; size_t p = 1;
    while (p < s.size() && (s[p] == '#' || s[p] == '_' || s[p] == 'b' || s[p] == 'x')) { semi += s[p] == 'b' ? -1 : s[p] == 'x' ? 2 : 1; p++; }
    if (p >= s.size()) return -1;
    bool neg = s[p] == '-'; if (neg) p++;
    if (p >= s.size() || !std::isdigit((unsigned char)s[p])) return -1;
    int oct = 0;
    while (p < s.size() && std::isdigit((unsigned char)s[p]) && oct < 100) oct = oct * 10 + (s[p++] - '0');
    if (p != s.size()) return -1;
    return ((neg ? -oct : oct) + 1) * 12 + semi;
}

// --- coll rows: the flat parts of a score, one event a row -----------------------------------------------------------
//   note at dur row pitch dyn tech gain        (pitch with the cents in it; "-" for no dynamics, technique, row)
//   sound at dur path from to gain             a region of a file
//   buffer at dur name from to gain            a region of a buffer
//   patch at dur name args...
//   msg at name values...                      (no duration)
// Scores placed inside are not rows: flatten first to have their events.
inline Datum symOrDash(const std::string& s) { return Datum::symbol(s.empty() ? "-" : s); }
inline std::vector<std::vector<Datum>> toRows(const Score& s) {
    std::vector<std::vector<Datum>> out;
    for (auto& ep : s.events()) {
        const Event& e = *ep;
        std::vector<Datum> r;
        switch (e.kind) {
        case Kind::Note:
            r = {Datum::symbol("note"), Datum::number(e.at), Datum::number(e.dur), symOrDash(e.row), Datum::number(e.sounding()),
                 symOrDash(e.dyn), symOrDash(e.tech), Datum::number(e.gain)};
            break;
        case Kind::Sound:
            r = {Datum::symbol(e.buffer ? "buffer" : "sound"), Datum::number(e.at), Datum::number(e.dur), Datum::string(e.source),
                 Datum::number(e.from), Datum::number(e.to), Datum::number(e.gain)};
            break;
        case Kind::Patch:
            r = {Datum::symbol("patch"), Datum::number(e.at), Datum::number(e.dur), Datum::string(e.source)};
            r.insert(r.end(), e.values.begin(), e.values.end());
            break;
        case Kind::Message:
            r = {Datum::symbol("msg"), Datum::number(e.at), Datum::symbol(e.source)};
            r.insert(r.end(), e.values.begin(), e.values.end());
            break;
        case Kind::Score: continue;
        }
        out.push_back(std::move(r));
    }
    return out;
}
// one row as an event (an Error names what is wrong)
inline std::shared_ptr<Event> eventOfRow(const std::vector<Datum>& r) {
    auto word = [&](size_t k) -> std::string {
        if (k >= r.size()) return "";
        if (r[k].kind == Datum::Str || r[k].kind == Datum::Sym) return r[k].s == "-" ? std::string() : r[k].s;
        if (r[k].kind == Datum::Num) { char b[32]; std::snprintf(b, sizeof b, "%g", r[k].num); return b; }
        throw Error("score: a row holds a list where a word is wanted");
    };
    auto num = [&](size_t k, double def) -> double {
        if (k >= r.size()) return def;
        if (r[k].kind == Datum::Num) { if (!std::isfinite(r[k].num)) throw Error("score: a number in a row is not finite"); return r[k].num; }
        if (r[k].kind == Datum::Sym || r[k].kind == Datum::Str) { double p = pitchOfName(r[k].s); if (p >= 0) return p; }
        throw Error("score: a row wants a number in place " + std::to_string(k + 1));
    };
    if (r.empty() || r[0].kind != Datum::Sym) throw Error("score: a row starts with note, sound, buffer, patch or msg");
    auto e = std::make_shared<Event>();
    const std::string& k = r[0].s;
    e->at = num(1, 0);
    if (k == "note") {
        e->kind = Kind::Note; e->dur = num(2, 0); e->row = word(3); e->pitch = num(4, 60); e->dyn = word(5); e->tech = word(6); e->gain = num(7, 1);
    } else if (k == "sound" || k == "buffer") {
        e->kind = Kind::Sound; e->buffer = k == "buffer"; e->dur = num(2, 0); e->source = word(3); e->from = num(4, 0); e->to = num(5, -1); e->gain = num(6, 1);
    } else if (k == "patch") {
        e->kind = Kind::Patch; e->dur = num(2, 0); e->source = word(3);
        e->values.assign(r.begin() + (long)std::min<size_t>(4, r.size()), r.end());
    } else if (k == "msg") {
        e->kind = Kind::Message; e->source = word(2);
        e->values.assign(r.begin() + (long)std::min<size_t>(3, r.size()), r.end());
    } else {
        throw Error("score: unknown row kind " + k);
    }
    if (e->at < 0 || e->dur < 0) throw Error("score: a row with a negative time");
    return e;
}

// --- writing: the score as Musil data, canonical (read back, it is the same score) ---------------------------------------
// (score "name" (sr 44100) (bpm 60) (meter 4 4) (next-id 9) (data key value)... (row name clef)...
//   (note at dur row pitch dyn tech options...)        options: (id n) (gain g) (az a) (el e) (cents c) (label "x")
//   (sound at dur "path" options...)                    (variant v) (data key value); sound: (from s) (to s);
//   (sound at dur (buffer "name") options...)           (row r) for every kind but notes
//   (patch at dur "name" (args values...) options...)
//   (msg at name values... options...)
//   (score at "file.score" (dur d) (stretch k) (transpose t) options...)   or with an embedded (score "name" ...)
// Numbers are written so that they read back exactly; a list value is (list ...).
inline std::string numText(double v) {
    if (v == 0) return "0";
    char b[40];
    if (std::fabs(v) < 1e15 && v == std::floor(v)) { std::snprintf(b, sizeof b, "%.0f", v); return b; }
    for (int prec = 1; prec <= 17; prec++) {
        std::snprintf(b, sizeof b, "%.*g", prec, v);
        if (std::strtod(b, nullptr) == v) break;
    }
    return b;
}
// a word written bare when Musil reads it back as that symbol, else as a string
inline bool bareWord(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if ((unsigned char)c <= ' ' || c == '(' || c == ')' || c == '{' || c == '}' || c == '"' || c == ';' || c == '\'' || c == '\\' || (unsigned char)c >= 127) return false;
    char* e = nullptr; std::strtod(s.c_str(), &e);
    return !(e && *e == '\0');           // a word that reads as a number is not a symbol
}
inline std::string quoted(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c == '\r') o += "\\r";
        else if (c == '\0') o += "\\0";
        else o += c;
    }
    return o + "\"";
}
inline std::string wordText(const std::string& s) { return bareWord(s) ? s : quoted(s); }
inline void writeDatum(const Datum& d, std::string& o) {
    switch (d.kind) {
    case Datum::Num: o += numText(d.num); return;
    case Datum::Str: o += quoted(d.s); return;
    case Datum::Sym: o += wordText(d.s); return;
    case Datum::List:
        o += "(list";
        for (auto& x : d.list) { o += ' '; writeDatum(x, o); }
        o += ')';
        return;
    }
}
inline void writeFields(const Fields& f, std::string& o) {
    for (auto& kv : f) { o += " (data " + wordText(kv.first) + ' '; writeDatum(kv.second, o); o += ')'; }
}
inline void writeScore(const Score& s, std::string& o, int indent);
inline void writeEvent(const Event& e, std::string& o, int indent) {
    auto opt = [&](const char* k, double v, double def) { if (v != def) o += std::string(" (") + k + ' ' + numText(v) + ')'; };
    o += '(';
    o += kindName(e.kind);
    o += ' ' + numText(e.at);
    if (e.kind != Kind::Message && e.kind != Kind::Score) o += ' ' + numText(e.dur);
    switch (e.kind) {
    case Kind::Note:
        o += ' ' + wordText(e.row) + ' ' + numText(e.pitch);
        if (!e.dyn.empty() || !e.tech.empty()) o += ' ' + wordText(e.dyn);
        if (!e.tech.empty()) o += ' ' + wordText(e.tech);
        break;
    case Kind::Sound:
        o += e.buffer ? " (buffer " + quoted(e.source) + ")" : ' ' + quoted(e.source);
        break;
    case Kind::Patch:
        o += ' ' + quoted(e.source) + " (args";
        for (auto& v : e.values) { o += ' '; writeDatum(v, o); }
        o += ')';
        break;
    case Kind::Message:
        o += ' ' + wordText(e.source);
        for (auto& v : e.values) { o += ' '; writeDatum(v, o); }
        break;
    case Kind::Score:
        if (!e.ref.empty()) o += ' ' + quoted(e.ref);
        else if (e.sub) { o += ' '; writeScore(*e.sub, o, indent + 2); }
        o += " (dur " + numText(e.dur) + ')';
        opt("stretch", e.stretch, 1);
        opt("transpose", e.transpose, 0);
        break;
    }
    o += " (id " + numText((double)e.id) + ')';
    if (e.kind == Kind::Message) opt("dur", e.dur, 0);
    opt("gain", e.gain, 1);
    opt("az", e.az, 0);
    opt("el", e.el, 0);
    if (e.kind == Kind::Note) opt("cents", e.cents, 0);
    if (e.kind == Kind::Sound) { opt("from", e.from, 0); opt("to", e.to, -1); }
    if (e.kind != Kind::Note && !e.row.empty()) o += " (row " + wordText(e.row) + ')';
    if (!e.variant.empty()) o += " (variant " + wordText(e.variant) + ')';
    if (!e.label.empty()) o += " (label " + quoted(e.label) + ')';
    writeFields(e.extra, o);
    o += ')';
}
inline void writeScore(const Score& s, std::string& o, int indent) {
    std::string pad((size_t)indent + 2, ' ');
    o += "(score " + quoted(s.name);
    o += "\n" + pad + "(sr " + numText(s.sr) + ") (bpm " + numText(s.bpm) + ") (meter " + std::to_string(s.meterNum) + ' ' +
         std::to_string(s.meterDen) + ") (next-id " + numText((double)s.nextId()) + ")";
    if (!s.extra.empty()) { o += "\n" + pad.substr(1); writeFields(s.extra, o); }
    for (auto& r : s.rows) {
        o += "\n" + pad + "(row " + wordText(r.name);
        if (!r.clef.empty()) o += ' ' + wordText(r.clef);
        writeFields(r.extra, o);
        o += ')';
    }
    for (auto& e : s.events()) { o += "\n" + pad; writeEvent(*e, o, indent + 2); }
    o += ')';
}
inline std::string toText(const Score& s) {
    std::string o = "; a score (Musil data: read with score-read, or opened by Moritz's score object)\n";
    writeScore(s, o, 0);
    return o + "\n";
}

}  // namespace score
}  // namespace musil
