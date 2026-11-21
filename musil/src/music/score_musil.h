// score_musil.h — the score (score.h) as a Musil value: music.mu's functions work on it.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// A score is an opaque value, (type s) "opaque:score", and so is an event of a score, "opaque:event": both
// answer as records (get, opt, has?, put!, pairs, keys), so the code written for the records music.mu used to
// make still works. Reading a key reads the C++ structure; setting one sets it there. The keys of an event:
//   kind        note, chord, file, buffer, synth, call, score, message, patch
//   at dur gain az el id label instr (the row)
//   midi pitch (the name) cents dyn tech            a note (a chord has instr and dyn)
//   source      a file's path; a score placed; a buffer, synth or call's vector or function (kept for Musil)
//   from to     a sound's region;  stretch transpose   a score's placement;  values   a patch or message
// Any other key is kept with the event: for Musil (a note's database and entry, a synth's parameters) and,
// when it is plain data (numbers, strings, symbols, lists of them), written to .score files; a database, a
// function or a vector is not written. The keys of a score: name sr bpm meter events next-id rows path, and
// any other (layout, reverb, ...) kept the same way. Events are listed in the order they were added; (get s
// 'events) makes a new list of them each time (push into it does not add one: event and score-add! do).
#pragma once
#include "../core.h"
#include "score.h"
#include "score_text.h"
#include "score_transform.h"
#include "score_layout.h"

#ifdef OPAQUE      // Windows (wingdi.h) defines it; Musil has a value type of that name
#undef OPAQUE
#endif

namespace musil {
namespace score_musil {

using score::Datum;
using score::Event;
using score::Kind;
using score::Score;

// a host payload: the Musil record of what an event or a score keeps for Musil
struct Host { vptr rec; };
inline vptr hostRec(const std::shared_ptr<void>& h) { return h ? static_cast<Host*>(h.get())->rec : nullptr; }
inline vptr& hostMake(std::shared_ptr<void>& h) {
    if (!h) h = std::make_shared<Host>(Host{v_list({})});
    return static_cast<Host*>(h.get())->rec;
}
// a payload of its own (a copy of the pairs): a copied event changed later does not change the original
inline std::shared_ptr<void> hostCopy(const std::shared_ptr<void>& h) {
    vptr r = hostRec(h);
    if (!r) return nullptr;
    vlist l;
    for (auto& p : r->l) l.push_back(p && p->t == Value::LIST && p->l.size() == 2 ? v_list({p->l[0], p->l[1]}) : p);
    return std::make_shared<Host>(Host{v_list(std::move(l))});
}
inline long hostFind(const vptr& rec, const vptr& key) {
    if (!rec) return -1;
    for (size_t k = 0; k < rec->l.size(); k++) { const vptr& p = rec->l[k]; if (p && p->t == Value::LIST && p->l.size() == 2 && equal(p->l[0], key)) return (long)k; }
    return -1;
}
inline void hostErase(std::shared_ptr<void>& h, const vptr& key) {
    vptr r = hostRec(h); long k = hostFind(r, key);
    if (k >= 0) r->l.erase(r->l.begin() + k);
}

// --- values and data ---------------------------------------------------------------------------------------------
inline bool toDatum(const vptr& v, Datum& out, int depth = 0) {
    if (!v || depth > 64) return false;
    switch (v->t) {
    case Value::NUM: if (v->num.size() != 1 || !std::isfinite(v->num[0])) return false; out = Datum::number(v->num[0]); return true;
    case Value::STR: out = Datum::string(v->s); return true;
    case Value::SYM: out = Datum::symbol(v->s); return true;
    case Value::LIST: {
        std::vector<Datum> l; l.reserve(v->l.size());
        for (auto& x : v->l) { Datum d; if (!toDatum(x, d, depth + 1)) return false; l.push_back(std::move(d)); }
        out = Datum::of(std::move(l)); return true;
    }
    default: return false;
    }
}
inline vptr fromDatum(const Datum& d) {
    switch (d.kind) {
    case Datum::Num: return v_num(d.num);
    case Datum::Str: return v_str(d.s);
    case Datum::Sym: return v_sym(d.s);
    case Datum::List: { vlist l; for (auto& x : d.list) l.push_back(fromDatum(x)); return v_list(std::move(l)); }
    }
    return v_nil();
}

// --- handles ---------------------------------------------------------------------------------------------------------
inline vptr v_score(std::shared_ptr<Score> s) { return v_opaque("score", std::move(s)); }
inline vptr v_event(std::shared_ptr<Event> e) { return v_opaque("event", std::move(e)); }
inline bool isScore(const vptr& v) { return v && v->t == Value::OPAQUE && v->opaque_tag == "score"; }
inline bool isEvent(const vptr& v) { return v && v->t == Value::OPAQUE && v->opaque_tag == "event"; }
inline std::shared_ptr<Score> scoreOf(Interp& i, const vptr& v) {
    if (!isScore(v)) i.bad("not a score: " + std::string(v && v->t == Value::OPAQUE ? "opaque:" + v->opaque_tag : type_name(v)));
    return std::static_pointer_cast<Score>(v->opaque);
}
inline std::shared_ptr<Event> eventOf(Interp& i, const vptr& v) {
    if (!isEvent(v)) i.bad("not an event of a score");
    return std::static_pointer_cast<Event>(v->opaque);
}
inline bool symIs(const vptr& k, const char* s) { return k && k->t == Value::SYM && k->s == s; }
inline double numOf(Interp& i, const vptr& v, const char* key) {
    if (!v || v->t != Value::NUM || v->num.size() != 1) i.bad(std::string(key) + " wants a number, not " + str_of_limited(v, 40));
    double d = v->num[0];
    if (!std::isfinite(d)) i.bad(std::string(key) + " wants a finite number");
    return d;
}
inline std::string textOf(const vptr& v) { return v && (v->t == Value::STR || v->t == Value::SYM) ? v->s : str_of_limited(v, 4096); }

// --- the keys of an event ----------------------------------------------------------------------------------------------
inline const char* musilKind(const Event& e) {
    switch (e.kind) {
    case Kind::Note: return e.variant == "chord" ? "chord" : "note";
    case Kind::Sound: return e.buffer ? "buffer" : "file";
    case Kind::Patch: return e.variant.empty() ? "patch" : e.variant.c_str();
    case Kind::Score: return "score";
    case Kind::Message: return "message";
    }
    return "note";
}
inline void setKind(Event& e, const std::string& k) {
    e.variant.clear(); e.buffer = false;
    if (k == "note") e.kind = Kind::Note;
    else if (k == "chord") { e.kind = Kind::Note; e.variant = "chord"; }
    else if (k == "file" || k == "sound") e.kind = Kind::Sound;
    else if (k == "buffer") { e.kind = Kind::Sound; e.buffer = true; }
    else if (k == "score") e.kind = Kind::Score;
    else if (k == "message" || k == "msg") e.kind = Kind::Message;
    else if (k == "patch") e.kind = Kind::Patch;
    else { e.kind = Kind::Patch; e.variant = k; }       // synth, call, or a kind of the user's
}
inline bool pitched(const Event& e) { return e.kind == Kind::Note && e.variant != "chord"; }
// tells the score holding the event that it changed
inline void touched(Event& e) { if (e.owner.p) e.owner.p->touch(); }

// a C++ name for a MIDI pitch (C4 = 60), as midi->pitch
inline std::string midiName(double p) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    long m = std::lround(p);
    long oct = m / 12 - 1, semi = m % 12; if (semi < 0) { semi += 12; oct--; }
    return std::string(names[semi]) + std::to_string(oct);
}

// the value of key in an event; found false when it has none
inline vptr eventGet(Interp& i, const std::shared_ptr<Event>& ep, const vptr& key, bool& found) {
    (void)i;
    Event& e = *ep;
    found = true;
    if (key && key->t == Value::SYM) {
        const std::string& k = key->s;
        if (k == "kind") return v_sym(musilKind(e));
        if (k == "at") return v_num(e.at);
        if (k == "dur") return v_num(e.dur);
        if (k == "gain") return v_num(e.gain);
        if (k == "az") return v_num(e.az);
        if (k == "el") return v_num(e.el);
        if (k == "id") return v_num((double)e.id);
        if (k == "label" && !e.label.empty()) return v_str(e.label);
        if ((k == "instr" || k == "row") && !e.row.empty()) return v_str(e.row);
        if (pitched(e)) {
            if (k == "midi") return v_num(e.pitch);
            if (k == "pitch") return v_str(midiName(e.pitch));
            if (k == "cents") return v_num(e.cents);
            if (k == "tech") return v_str(e.tech);
        }
        if (e.kind == Kind::Note && k == "dyn") return v_str(e.dyn);
        if (e.kind == Kind::Score) {
            if (k == "source") return e.sub ? v_score(e.sub) : v_nil();
            if (k == "stretch") return v_num(e.stretch);
            if (k == "transpose") return v_num(e.transpose);
        }
        if (e.kind == Kind::Sound) {
            if (k == "from") return v_num(e.from);
            if (k == "to") return v_num(e.to);
        }
        if ((e.kind == Kind::Patch || e.kind == Kind::Message) && k == "values" && !e.values.empty()) {
            vlist l; for (auto& d : e.values) l.push_back(fromDatum(d)); return v_list(std::move(l));
        }
        if (k == "source" && e.kind != Kind::Note) {
            long h = hostFind(hostRec(e.host), key);
            if (h >= 0) return hostRec(e.host)->l[(size_t)h]->l[1];
            if (!e.source.empty()) return v_str(e.source);
        }
        if (auto* d = score::field(e.extra, k)) return fromDatum(*d);
    }
    vptr r = hostRec(e.host);
    long h = hostFind(r, key);
    if (h >= 0) return r->l[(size_t)h]->l[1];
    found = false;
    return v_nil();
}
inline void eventPut(Interp& i, const std::shared_ptr<Event>& ep, const vptr& key, const vptr& v) {
    Event& e = *ep;
    if (key && key->t == Value::SYM) {
        const std::string& k = key->s;
        bool done = true;
        if (k == "kind") { if (e.owner.p) i.bad("the kind of an event in a score cannot change"); setKind(e, textOf(v)); }
        else if (k == "at") e.at = numOf(i, v, "at");
        else if (k == "dur") { double d = numOf(i, v, "dur"); if (d < 0) i.bad("dur must be >= 0"); e.dur = d; }
        else if (k == "gain") e.gain = numOf(i, v, "gain");
        else if (k == "az") e.az = numOf(i, v, "az");
        else if (k == "el") e.el = numOf(i, v, "el");
        else if (k == "id") {
            if (e.owner.p) i.bad("the id of an event in a score cannot change");
            double d = numOf(i, v, "id"); if (d < 0 || d != std::floor(d) || d > 9007199254740992.0) i.bad("id wants a whole number >= 0");
            e.id = (uint64_t)d;
        }
        else if (k == "label" && v && v->t == Value::STR) { e.label = v->s; hostErase(e.host, key); }
        else if (k == "instr" || k == "row") e.row = textOf(v);
        else if (pitched(e) && k == "midi") e.pitch = numOf(i, v, "midi");
        else if (pitched(e) && k == "pitch") {
            if (v && v->t == Value::NUM) e.pitch = numOf(i, v, "pitch");
            else { double m = score::pitchOfName(textOf(v)); if (m < 0) i.bad("pitch: not a pitch: " + textOf(v)); e.pitch = m; }
        }
        else if (pitched(e) && k == "cents") e.cents = numOf(i, v, "cents");
        else if (pitched(e) && k == "tech") e.tech = textOf(v);
        else if (e.kind == Kind::Note && k == "dyn") e.dyn = textOf(v);
        else if (e.kind == Kind::Score && k == "source") {
            auto s = scoreOf(i, v);
            Score* own = e.owner.p;
            if (own && (s.get() == own || s->contains(own))) i.bad("a score cannot contain itself");
            e.sub = s; e.ref.clear();
        }
        else if (e.kind == Kind::Score && k == "stretch") { double d = numOf(i, v, "stretch"); if (d <= 0) i.bad("stretch must be > 0"); e.stretch = d; }
        else if (e.kind == Kind::Score && k == "transpose") e.transpose = numOf(i, v, "transpose");
        else if (e.kind == Kind::Sound && k == "from") e.from = numOf(i, v, "from");
        else if (e.kind == Kind::Sound && k == "to") e.to = numOf(i, v, "to");
        else if ((e.kind == Kind::Sound || e.kind == Kind::Patch || e.kind == Kind::Message) && k == "source" && v && v->t == Value::STR) {
            e.source = v->s; hostErase(e.host, key);
        }
        else if ((e.kind == Kind::Patch || e.kind == Kind::Message) && k == "values" && v && v->t == Value::LIST) {
            std::vector<Datum> l; Datum d;
            for (auto& x : v->l) { if (!toDatum(x, d)) i.bad("values: numbers, strings, symbols or lists of them"); l.push_back(d); }
            e.values = std::move(l);
        }
        else if (score::field(e.extra, k)) {          // a field read from a file: it stays one while it is data
            Datum d;
            if (toDatum(v, d)) score::setField(e.extra, k, std::move(d));
            else { score::eraseField(e.extra, k); done = false; }
        }
        else done = false;
        if (done) { touched(e); return; }
    }
    vptr& r = hostMake(e.host);
    long h = hostFind(r, key);
    if (h >= 0) r->l[(size_t)h]->l[1] = v; else r->l.push_back(v_list({key, v}));
    touched(e);
}
inline vptr eventPairs(Interp& i, const std::shared_ptr<Event>& ep) {
    const Event& e = *ep;
    vlist out;
    auto add = [&](const char* k) {
        bool f = false; vptr v = eventGet(i, ep, v_sym(k), f);
        if (f) out.push_back(v_list({v_sym(k), v}));
    };
    add("kind"); add("source"); add("label"); add("instr");
    if (pitched(e)) { add("pitch"); add("midi"); }
    if (e.kind == Kind::Note) add("dyn");
    if (pitched(e)) { add("tech"); add("cents"); }
    add("at"); add("dur"); add("az"); add("el"); add("gain"); add("id");
    if (e.kind == Kind::Sound) { if (e.from != 0) add("from"); if (e.to >= 0) add("to"); }
    if (e.kind == Kind::Score) { if (e.stretch != 1) add("stretch"); if (e.transpose != 0) add("transpose"); }
    add("values");
    for (auto& kv : e.extra) out.push_back(v_list({v_sym(kv.first), fromDatum(kv.second)}));
    if (vptr r = hostRec(e.host))
        for (auto& p : r->l) {
            if (!p || p->t != Value::LIST || p->l.size() != 2) continue;
            if (symIs(p->l[0], "source")) { bool dup = false; for (auto& o : out) if (symIs(o->l[0], "source")) dup = true; if (dup) continue; }
            out.push_back(v_list({p->l[0], p->l[1]}));
        }
    return v_list(std::move(out));
}
// a new event from a record (or a copy of another event); its id kept when it has one
inline std::shared_ptr<Event> eventOfRecord(Interp& i, const vptr& rec) {
    if (isEvent(rec)) {
        auto c = std::make_shared<Event>(*eventOf(i, rec));
        c->host = hostCopy(c->host);
        return c;
    }
    if (!rec || rec->t != Value::LIST) i.bad("an event is a record: (list (list 'kind ...) (list 'at ...) ...)");
    auto e = std::make_shared<Event>();
    vptr kindKey = v_sym("kind");
    long k = hostFind(rec, kindKey);
    if (k < 0) i.bad("an event needs its kind");
    setKind(*e, textOf(rec->l[(size_t)k]->l[1]));
    bool hasMidi = hostFind(rec, v_sym("midi")) >= 0;
    for (auto& p : rec->l) {
        if (!p || p->t != Value::LIST || p->l.size() != 2) i.bad("a record is a list of (key value) pairs");
        if (symIs(p->l[0], "kind")) continue;
        if (hasMidi && pitched(*e) && symIs(p->l[0], "pitch")) continue;   // the name follows the number
        eventPut(i, e, p->l[0], p->l[1]);
    }
    if (e->kind == Kind::Score && !e->sub) i.bad("a score event needs the score it places (source)");
    return e;
}

// --- the keys of a score --------------------------------------------------------------------------------------------------
inline vptr scoreGet(Interp& i, const std::shared_ptr<Score>& sp, const vptr& key, bool& found) {
    (void)i;
    Score& s = *sp;
    found = true;
    if (key && key->t == Value::SYM) {
        const std::string& k = key->s;
        if (k == "name") return v_str(s.name);
        if (k == "sr") return v_num(s.sr);
        if (k == "bpm") return v_num(s.bpm);
        if (k == "meter") return v_list({v_num(s.meterNum), v_num(s.meterDen)});
        if (k == "next-id") return v_num((double)s.nextId());
        if (k == "events") { vlist l; l.reserve(s.size()); for (auto& e : s.events()) l.push_back(v_event(e)); return v_list(std::move(l)); }
        if (k == "rows" && !s.rows.empty()) {
            vlist l; for (auto& r : s.rows) l.push_back(v_list({v_str(r.name), v_str(r.clef)})); return v_list(std::move(l));
        }
        if (k == "path" && !s.path.empty()) return v_str(s.path);
        if (auto* d = score::field(s.extra, k)) return fromDatum(*d);
    }
    vptr r = hostRec(s.host);
    long h = hostFind(r, key);
    if (h >= 0) return r->l[(size_t)h]->l[1];
    found = false;
    return v_nil();
}
inline void assignEvents(Interp& i, Score& s, const vptr& list) {
    if (!list || list->t != Value::LIST) i.bad("events: a list of events");
    std::vector<std::shared_ptr<Event>> evs; evs.reserve(list->l.size());
    for (auto& x : list->l) {
        if (isEvent(x)) {
            auto e = eventOf(i, x);
            if (e->owner.p && e->owner.p != &s) { auto c = std::make_shared<Event>(*e); c->host = hostCopy(c->host); evs.push_back(c); }
            else evs.push_back(e);
        } else evs.push_back(eventOfRecord(i, x));
    }
    try { s.assign(std::move(evs)); } catch (const score::Error& e) { i.bad(e.what()); }
}
inline void scorePut(Interp& i, const std::shared_ptr<Score>& sp, const vptr& key, const vptr& v) {
    Score& s = *sp;
    if (key && key->t == Value::SYM) {
        const std::string& k = key->s;
        if (k == "name") { s.name = textOf(v); return; }
        if (k == "sr") { double d = numOf(i, v, "sr"); if (d <= 0) i.bad("sr must be > 0"); s.sr = d; return; }
        if (k == "bpm") { double d = numOf(i, v, "bpm"); if (d <= 0) i.bad("bpm must be > 0"); s.bpm = d; return; }
        if (k == "meter") {
            if (!v || v->t != Value::LIST || v->l.size() != 2) i.bad("meter: (list num den)");
            double n = numOf(i, v->l[0], "meter"), d = numOf(i, v->l[1], "meter");
            if (n < 1 || d < 1 || n > 1000 || d > 1024) i.bad("meter: num 1 to 1000, den 1 to 1024");
            s.meterNum = (int)n; s.meterDen = (int)d; return;
        }
        if (k == "next-id") { s.setNextId((uint64_t)std::max(1.0, numOf(i, v, "next-id"))); return; }
        if (k == "events") { assignEvents(i, s, v); return; }
        if (k == "rows" && v && v->t == Value::LIST) {
            std::vector<score::Row> rows;
            for (auto& r : v->l) {
                score::Row row;
                if (r && r->t == Value::LIST && !r->l.empty()) { row.name = textOf(r->l[0]); if (r->l.size() > 1) row.clef = textOf(r->l[1]); }
                else row.name = textOf(r);
                rows.push_back(std::move(row));
            }
            s.rows = std::move(rows); return;
        }
        if (score::field(s.extra, k)) {
            Datum d;
            if (toDatum(v, d)) { score::setField(s.extra, k, std::move(d)); return; }
            score::eraseField(s.extra, k);
        }
    }
    vptr& r = hostMake(s.host);
    long h = hostFind(r, key);
    if (h >= 0) r->l[(size_t)h]->l[1] = v; else r->l.push_back(v_list({key, v}));
}
inline vptr scorePairs(Interp& i, const std::shared_ptr<Score>& sp) {
    vlist out;
    for (const char* k : {"name", "sr", "events", "bpm", "meter", "next-id", "rows", "path"}) {
        bool f = false; vptr v = scoreGet(i, sp, v_sym(k), f);
        if (f) out.push_back(v_list({v_sym(k), v}));
    }
    for (auto& kv : sp->extra) out.push_back(v_list({v_sym(kv.first), fromDatum(kv.second)}));
    if (vptr r = hostRec(sp->host))
        for (auto& p : r->l) if (p && p->t == Value::LIST && p->l.size() == 2) out.push_back(v_list({p->l[0], p->l[1]}));
    return v_list(std::move(out));
}

// --- what is written: Musil's own keys stay with Musil ------------------------------------------------------------------------
// the keys made again from a database (or that only Musil can hold) are not written
inline bool musilOnly(const std::string& k) {
    return k == "db" || k == "entry" || k == "shift" || k == "notes" || k == "source" || k == "cache" || k == "orchestrations";
}
inline void hostToExtra(const std::shared_ptr<void>& host, score::Fields& extra) {
    vptr r = hostRec(host);
    if (!r) return;
    for (auto& p : r->l) {
        if (!p || p->t != Value::LIST || p->l.size() != 2) continue;
        const vptr& k = p->l[0];
        if (!k || k->t != Value::SYM || musilOnly(k->s)) continue;
        Datum d;
        if (toDatum(p->l[1], d)) score::setField(extra, k->s, std::move(d));
    }
}
// a copy of a score for elsewhere (a file, another program, another thread): its events and the scores in it copied,
// what Musil keeps for itself left out (what is plain data among it becomes the extra fields). A chord becomes its
// notes (the first keeps its id); an event whose sound only Musil holds (a buffer's vector, a synth's or a call's
// function) is left out and counted in leftOut. The same score placed twice stays one score placed twice.
inline std::shared_ptr<Score> detached(const Score& s, std::map<const Score*, std::shared_ptr<Score>>& done, size_t* leftOut, int depth = 0) {
    auto hit = done.find(&s);
    if (hit != done.end()) return hit->second;
    if (depth > score::kMaxDepth) throw score::Error("score: scores nested more than " + std::to_string(score::kMaxDepth) + " deep");
    auto c = s.clone();
    hostToExtra(s.host, c->extra);
    c->host.reset();
    std::vector<std::shared_ptr<Event>> evs;
    uint64_t next = c->nextId();
    for (auto& e : c->events()) {
        bool musilSound = (e->kind == Kind::Sound && e->buffer && e->source.empty()) ||
                          (e->kind == Kind::Patch && e->source.empty() && (e->variant == "synth" || e->variant == "call"));
        if (musilSound) { if (leftOut) ++*leftOut; continue; }
        if (e->kind == Kind::Note && e->variant == "chord") {       // its notes, each a note of the score
            vptr r = hostRec(e->host);
            long h = hostFind(r, v_sym("notes"));
            bool first = true;
            if (h >= 0 && r->l[(size_t)h]->l[1]->t == Value::LIST)
                for (auto& n : r->l[(size_t)h]->l[1]->l) {
                    long m = hostFind(n, v_sym("midi"));
                    if (m < 0) continue;
                    const vptr& mv = n->l[(size_t)m]->l[1];
                    if (mv->t != Value::NUM || mv->num.size() != 1) continue;
                    auto x = std::make_shared<Event>(*e);
                    x->variant.clear();
                    x->label.clear();
                    x->host.reset();
                    x->pitch = mv->num[0];
                    long t = hostFind(n, v_sym("tech"));
                    if (t >= 0) x->tech = textOf(n->l[(size_t)t]->l[1]);
                    x->id = first ? e->id : next++;
                    first = false;
                    evs.push_back(x);
                }
            if (first && leftOut) ++*leftOut;
            continue;
        }
        auto x = std::make_shared<Event>(*e);
        hostToExtra(e->host, x->extra);
        x->host.reset();
        if (x->kind == Kind::Score && x->sub) x->sub = detached(*x->sub, done, leftOut, depth + 1);
        evs.push_back(x);
    }
    c->assign(std::move(evs));
    c->setNextId(std::max(next, c->nextId()));
    done[&s] = c;
    return c;
}
inline std::shared_ptr<Score> detached(const Score& s, size_t* leftOut = nullptr) {
    std::map<const Score*, std::shared_ptr<Score>> done;
    return detached(s, done, leftOut);
}

// the score's form as Musil data: what a .score file holds, read back as a list (no evaluation)
inline vptr dataOf(const Score& s) {
    std::string text = score::toText(*detached(s));
    Parser p(text, std::make_shared<std::string>("<score>"));
    p.literal = false;
    vptr prog = p.program();
    return prog->l.size() == 2 ? prog->l[1] : v_nil();
}

// --- builtins ------------------------------------------------------------------------------------------------------------------------
inline std::shared_ptr<Score> S(Interp& i, const vptr& v) { return scoreOf(i, v); }
template <class F> auto guarded(Interp& i, F f) -> decltype(f()) {
    try { return f(); } catch (const score::Error& e) { i.bad(e.what()); }
}
// what a .score file read from Musil may open: the files it places, through the interpreter's capabilities
inline score::ReadContext readContext(Interp& i) {
    score::ReadContext c;
    c.readFile = [&i](const std::string& path) { i.require_read(path); return score::readWholeFile(path); };
    return c;
}

// (score-make name sr)     a new score, empty (music.mu's score adds the layout and the reverb)
inline vptr mus_score_make(vlist& a, Interp& i) {
    double sr = i.scalar(a[1]);
    if (!(sr > 0) || sr > 1e7) i.bad("the rate must be > 0");
    return v_score(std::make_shared<Score>(textOf(a[0]), sr));
}
// (score? x)               is x a score?
inline vptr mus_scorep(vlist& a, Interp&) { return v_bool(isScore(a[0])); }
// (score-add! s e)         an event (a record, or an event to copy) added at the end of the score; its id kept when it
//                          has one not yet used, else a new one; a score that would contain itself, directly or through
//                          the scores inside it, is refused. => the event in the score
inline vptr mus_score_add(vlist& a, Interp& i) {
    auto s = S(i, a[0]);
    std::shared_ptr<Event> e;
    if (isEvent(a[1]) && !eventOf(i, a[1])->owner.p) e = eventOf(i, a[1]);   // an event of no score: it joins this one
    else e = eventOfRecord(i, a[1]);
    guarded(i, [&] { return s->insert(e); });
    return v_event(e);
}
// (score-take-id! s)       a fresh event id of the score: never used again, even after events are removed
inline vptr mus_score_take_id(vlist& a, Interp& i) { return v_num((double)S(i, a[0])->takeId()); }
// (score-duration s)       when its last event ends (0 when empty)
inline vptr mus_score_duration(vlist& a, Interp& i) { return v_num(S(i, a[0])->duration()); }
// (score-find s id)        the event of an id, or nil
inline vptr mus_score_find(vlist& a, Interp& i) {
    auto e = S(i, a[0])->find((uint64_t)std::max(0.0, i.scalar(a[1])));
    return e ? v_event(e) : v_nil();
}
// (score-remove-id! s id)  the event of an id taken out of the score; => it (nil when there was none)
inline vptr mus_score_remove_id(vlist& a, Interp& i) {
    auto e = S(i, a[0])->remove((uint64_t)std::max(0.0, i.scalar(a[1])));
    return e ? v_event(e) : v_nil();
}
// (score-query s t0 t1)    the events sounding between t0 and t1 seconds (starting before t1, ending after t0), by onset
inline vptr mus_score_query(vlist& a, Interp& i) {
    auto s = S(i, a[0]);
    vlist out;
    s->query(i.scalar(a[1]), i.scalar(a[2]), [&](const Event& e) { out.push_back(v_event(s->find(e.id))); });
    return v_list(std::move(out));
}
// (score-copy s)           a copy of the score: its events copied (the same ids), the scores placed in it shared
inline vptr mus_score_copy(vlist& a, Interp& i) {
    auto s = S(i, a[0]);
    auto c = s->clone();
    c->host = hostCopy(s->host);
    for (auto& e : c->events()) e->host = hostCopy(e->host);
    return v_score(c);
}
// the events of a flattened score as Musil events: a note transposed by its placements is shifted (its 'shift, the
// resampling of its sound) by as much
inline vptr flatList(Interp& i, const Score& s, int depth) {
    std::vector<score::Placed> flat;
    guarded(i, [&] { score::flatten(s, flat, depth); return 0; });
    vlist out; out.reserve(flat.size());
    size_t top = 0;
    for (auto& p : flat) {
        if (p.depth == depth) {                                   // an event of the score itself: the event, not a copy
            while (top < s.size() && s.events()[top]->kind == Kind::Score) top++;
            if (top < s.size()) { out.push_back(v_event(s.events()[top++])); continue; }
        }
        auto e = std::make_shared<Event>(std::move(p.ev));
        e->host = hostCopy(e->host);
        if (p.transpose != 0 && pitched(*e)) {
            vptr r = hostRec(e->host); vptr k = v_sym("shift"); long h = hostFind(r, k);
            if (h >= 0 && r->l[(size_t)h]->l[1]->t == Value::NUM) r->l[(size_t)h]->l[1] = v_num(r->l[(size_t)h]->l[1]->num[0] + p.transpose);
        }
        out.push_back(v_event(e));
    }
    return v_list(std::move(out));
}
// (score-flatten s depth)  the score's events with those of the scores inside brought to the top: copies at absolute times,
//                          stretched, transposed, their gains multiplied and their azimuths turned by their placements, cut
//                          to their score's event (the events of s itself are given as they are); depth is how deep s is
//                          (0 at the top: deeper than 16 is an error)
inline vptr mus_score_flatten(vlist& a, Interp& i) { return flatList(i, *S(i, a[0]), (int)i.scalar(a[1])); }
// (score-placed s stretch transpose az el)   a copy of s as a placement makes it sound: times and durations stretched,
//                          notes transposed (their sounds shifted), directions turned
inline vptr mus_score_placed(vlist& a, Interp& i) {
    auto s = S(i, a[0]);
    double k = i.scalar(a[1]), t = i.scalar(a[2]), az = i.scalar(a[3]), el = i.scalar(a[4]);
    if (!(k > 0)) i.bad("the stretch must be > 0");
    auto c = s->clone();
    c->host = hostCopy(s->host);
    for (auto& e : c->events()) {
        e->host = hostCopy(e->host);
        e->at *= k; e->dur *= k; e->az += az; e->el = std::max(-90.0, std::min(90.0, e->el + el));
        if (e->kind == Kind::Score) e->transpose += t;
        else if (pitched(*e) && t != 0) {
            e->pitch += t;
            vptr r = hostRec(e->host); vptr key = v_sym("shift"); long h = hostFind(r, key);
            if (h >= 0 && r->l[(size_t)h]->l[1]->t == Value::NUM) r->l[(size_t)h]->l[1] = v_num(r->l[(size_t)h]->l[1]->num[0] + t);
        }
    }
    c->touch();
    return v_score(c);
}
// (score-read path)        a .score file: the score it holds (read as data, nothing run); the scores it places by file
//                          are read too, each once (placed three times, it is one score with three placements)
inline vptr mus_score_read(vlist& a, Interp& i) {
    std::string path = i.readable(i.str(a[0]));
    return v_score(guarded(i, [&] { return score::readFile(path, readContext(i)); }));
}
// (score-write s path)     the score as a .score file (Musil data); what only Musil can hold (a database, a function, a
//                          vector) is left out (an event made of it entirely: said), a chord is written as its notes, a
//                          score placed from a file is written by its name; => the path
inline vptr mus_score_write(vlist& a, Interp& i) {
    auto s = S(i, a[0]);
    std::string path = i.writable(i.str(a[1]));
    size_t left = 0;
    guarded(i, [&] { score::writeFile(*detached(*s, &left), path); return 0; });
    if (left) *i.out << "score-write: " << left << " events whose sound only Musil holds (a buffer's vector, a synth, a call) left out" << std::endl;
    s->path = path;
    return v_str(path);
}
// (score->text s)          what score-write writes, as a string
inline vptr mus_score_text(vlist& a, Interp& i) { return v_str(guarded(i, [&] { return score::toText(*detached(*S(i, a[0]))); })); }
// (text->score text)       a score from the text of a .score file
inline vptr mus_text_score(vlist& a, Interp& i) {
    return v_score(guarded(i, [&] { return score::readText(i.str(a[0]), "<text>", readContext(i)); }));
}
// (score->data s)          the score as Musil data, the list of lists a .score file holds: (score "name" (sr 44100) ...)
inline vptr mus_score_data(vlist& a, Interp& i) { return guarded(i, [&] { return dataOf(*S(i, a[0])); }); }
// (data->score form)       a score from its form as data (nothing is evaluated)
inline vptr mus_data_score(vlist& a, Interp& i) {
    return v_score(guarded(i, [&] { return score::fromData(a[0], i.write_dir, readContext(i)); }));
}
// (score->coll s)          the score's flat parts, one event a row, as lists: (note at dur row pitch dyn tech gain),
//                          (sound at dur path from to gain), (buffer at dur name from to gain), (patch at dur name args...),
//                          (msg at name values...); the scores placed in it are not rows (flatten first)
inline vptr mus_score_coll(vlist& a, Interp& i) {
    auto s = detached(*S(i, a[0]));
    vlist out;
    for (auto& r : score::toRows(*s)) { vlist l; for (auto& d : r) l.push_back(fromDatum(d)); out.push_back(v_list(std::move(l))); }
    return v_list(std::move(out));
}
// (coll->score name sr rows)   a score made of rows as score->coll gives them (pitches may be names: G5)
inline vptr mus_coll_score(vlist& a, Interp& i) {
    auto s = std::make_shared<Score>(textOf(a[0]), i.scalar(a[1]));
    if (!(s->sr > 0)) i.bad("the rate must be > 0");
    for (auto& r : i.list(a[2])) {
        if (!r || r->t != Value::LIST) i.bad("a row is a list");
        std::vector<Datum> row; Datum d;
        for (auto& x : r->l) { if (!toDatum(x, d)) i.bad("a row holds numbers, strings and symbols"); row.push_back(d); }
        guarded(i, [&] { return s->insert(score::eventOfRow(row)); });
    }
    return v_score(s);
}
// (score-pan-gains az n)   the gains of a mono sound at an azimuth on n channels, as a score places it: 1 channel all of
//                          it, 2 equal power between left and right (pan-azimuth), more a ring of n speakers (pan-n on
//                          speaker-ring n)
inline vptr mus_score_pan_gains(vlist& a, Interp& i) {
    int n = (int)i.scalar(a[1]);
    if (n < 1 || n > 1024) i.bad("1 to 1024 channels");
    std::vector<double> g((size_t)n);
    score::panGains(i.scalar(a[0]), n, g.data());
    varr v((size_t)n); for (int k = 0; k < n; k++) v[(size_t)k] = g[(size_t)k];
    return v_arr(std::move(v));
}
// (score-bbt s seconds)    a time as (list bar beat tick) at the score's tempo and meter (from 1; 480 ticks a beat)
inline vptr mus_score_bbt(vlist& a, Interp& i) {
    auto s = S(i, a[0]);
    auto p = score::toBBT(i.scalar(a[1]), s->bpm, s->meterNum);
    return v_list({v_num((double)p.bar), v_num(p.beat), v_num(p.tick)});
}
// (score-bbt->sec s bar beat tick)   a position in bars, beats (from 1) and ticks as seconds at the score's tempo and meter
inline vptr mus_score_bbt_sec(vlist& a, Interp& i) {
    auto s = S(i, a[0]);
    return v_num(score::fromBBT(i.scalar(a[1]), i.scalar(a[2]), i.scalar(a[3]), s->bpm, s->meterNum));
}

// --- transformations (score_transform.h): the same functions Moritz's roll and score object call ---------------------------------------
// the ids an optional argument names: a list of ids or of events of the score (none: every event)
inline score::transform::Ids idsArg(Interp& i, vlist& a, size_t k) {
    score::transform::Ids ids;
    if (a.size() <= k) return ids;
    const vptr& v = a[k];
    if (isEvent(v)) { ids.push_back(eventOf(i, v)->id); return ids; }
    if (v && v->t == Value::NUM) { for (double d : v->num) ids.push_back((uint64_t)std::max(0.0, d)); }
    else for (auto& x : i.list(v)) {
        if (isEvent(x)) ids.push_back(eventOf(i, x)->id);
        else ids.push_back((uint64_t)std::max(0.0, i.scalar(x)));
    }
    if (ids.empty()) ids.push_back(0);            // an empty list selects nothing (id 0 is never an event's)
    return ids;
}
// a pitch: a MIDI number or a name (C4, "F#3")
inline double pitchArg(Interp& i, const vptr& v) {
    if (v && (v->t == Value::STR || v->t == Value::SYM)) {
        double p = score::pitchOfName(v->s);
        if (p < 0) i.bad("not a pitch: " + v->s);
        return p;
    }
    return i.scalar(v);
}
// a pitch transformation, with the sounds of Musil's notes shifted by as much as their pitches moved
template <class F> inline void withShifts(Score& s, F f) {
    std::map<uint64_t, double> before;
    for (auto& e : s.events()) if (pitched(*e)) before[e->id] = e->pitch;
    f();
    for (auto& e : s.events()) {
        auto it = before.find(e->id);
        if (it == before.end() || e->pitch == it->second) continue;
        vptr r = hostRec(e->host); long h = hostFind(r, v_sym("shift"));
        if (h >= 0 && r->l[(size_t)h]->l[1]->t == Value::NUM) r->l[(size_t)h]->l[1] = v_num(r->l[(size_t)h]->l[1]->num[0] + e->pitch - it->second);
    }
}
inline vptr idList(const score::transform::Ids& ids) {
    varr v(ids.size());
    for (size_t k = 0; k < ids.size(); k++) v[k] = (double)ids[k];
    return v_arr(std::move(v));
}
// (score-transpose! s n [ids])   the notes (and the scores placed) n semitones higher; ids: a list of ids or events
//                          (none: every event). => s
inline vptr mus_score_transpose(vlist& a, Interp& i) {
    auto s = S(i, a[0]); double n = i.scalar(a[1]); auto ids = idsArg(i, a, 2);
    withShifts(*s, [&] { score::transform::transpose(*s, n, ids); });
    return a[0];
}
// (score-invert! s axis [ids])   the notes mirrored around a pitch (a number or a name); twice gives them back. => s
inline vptr mus_score_invert(vlist& a, Interp& i) {
    auto s = S(i, a[0]); double axis = pitchArg(i, a[1]); auto ids = idsArg(i, a, 2);
    withShifts(*s, [&] { score::transform::invert(*s, axis, ids); });
    return a[0];
}
// (score-retrograde! s [ids])    the events backwards within their span (from the first onset to the last end). => s
inline vptr mus_score_retrograde(vlist& a, Interp& i) {
    auto s = S(i, a[0]);
    score::transform::retrograde(*s, idsArg(i, a, 1));
    return a[0];
}
// (score-augment! s k [ids]) (score-diminish! s k [ids])   onsets and durations stretched by k (divided by k) from the
//                          first onset of the events. => s
inline vptr mus_score_augment(vlist& a, Interp& i) {
    auto s = S(i, a[0]); double k = i.scalar(a[1]); auto ids = idsArg(i, a, 2);
    guarded(i, [&] { return score::transform::augment(*s, k, ids); });
    return a[0];
}
inline vptr mus_score_diminish(vlist& a, Interp& i) {
    auto s = S(i, a[0]); double k = i.scalar(a[1]); auto ids = idsArg(i, a, 2);
    if (!(k > 0)) i.bad("the factor must be > 0");
    guarded(i, [&] { return score::transform::augment(*s, 1 / k, ids); });
    return a[0];
}
// (score-quantize-grid! s grid strength [ids])   onsets and ends moved toward the nearest multiple of grid seconds (strength 1:
//                          onto it, 0.5: half way); an end that would reach its onset is a grid after it. => s
inline vptr mus_score_quantize_grid(vlist& a, Interp& i) {
    auto s = S(i, a[0]); double g = i.scalar(a[1]), k = i.scalar(a[2]); auto ids = idsArg(i, a, 3);
    guarded(i, [&] { return score::transform::quantize(*s, g, k, ids); });
    return a[0];
}
// (score-humanize! s seconds db seed [ids])   each onset moved at random by up to seconds either way, each gain by up to db
//                          decibels; the same seed, the same result on every system. => s
inline vptr mus_score_humanize(vlist& a, Interp& i) {
    auto s = S(i, a[0]);
    score::transform::humanize(*s, i.scalar(a[1]), i.scalar(a[2]), (uint32_t)(int64_t)i.scalar(a[3]), idsArg(i, a, 4));
    return a[0];
}
// (score-thin! s keep seed [ids])   each event kept with probability keep, the others taken out. => the ids taken out
inline vptr mus_score_thin(vlist& a, Interp& i) {
    auto s = S(i, a[0]);
    return idList(score::transform::thin(*s, i.scalar(a[1]), (uint32_t)(int64_t)i.scalar(a[2]), idsArg(i, a, 3)));
}
// (score-density! s curve seed [ids])   each event kept with the probability a curve (a vector or a list of values 0 to 1,
//                          read along the events' span) has at its onset. => the ids taken out
inline vptr mus_score_density(vlist& a, Interp& i) {
    auto s = S(i, a[0]);
    std::vector<double> curve;
    if (a[1] && a[1]->t == Value::NUM) for (double d : a[1]->num) curve.push_back(d);
    else for (auto& x : i.list(a[1])) curve.push_back(i.scalar(x));
    uint32_t seed = (uint32_t)(int64_t)i.scalar(a[2]);
    auto ids = idsArg(i, a, 3);
    return idList(guarded(i, [&] { return score::transform::density(*s, curve, seed, ids); }));
}
// (score-shift! s dt [ids])     every onset moved by dt seconds (never before 0). => s
inline vptr mus_score_shift(vlist& a, Interp& i) {
    auto s = S(i, a[0]);
    score::transform::shift(*s, i.scalar(a[1]), idsArg(i, a, 2));
    return a[0];
}
// (score-scale! s f)            every onset and duration multiplied by f (> 0). => s
inline vptr mus_score_scale(vlist& a, Interp& i) {
    auto s = S(i, a[0]); double f = i.scalar(a[1]);
    guarded(i, [&] { return score::transform::augment(*s, f, 0.0, {}); });
    return a[0];
}

// --- layout (score_layout.h): the rules Moritz's roll draws with --------------------------------------------------------------------
// (instrument-rank instr)  where an instrument code sits in the orchestral order (Picc Fl ... Vc Cb; unknown ones after)
inline vptr mus_instrument_rank(vlist& a, Interp&) { return v_num(score::layout::instrumentRank(textOf(a[0]))); }
// (score-orchestral-order) the instrument codes in the order of an orchestral score
inline vptr mus_orchestral_order(vlist&, Interp&) {
    vlist out;
    for (auto& n : score::layout::orchestralOrder()) out.push_back(v_str(n));
    return v_list(std::move(out));
}
// (staff-clef pitches)     the clef of a row of these MIDI pitches: "bass" when their median is below C4, else "treble";
//                          "none" without pitches
inline vptr mus_staff_clef(vlist& a, Interp& i) {
    std::vector<double> p;
    if (a[0] && a[0]->t == Value::NUM) for (double d : a[0]->num) p.push_back(d);
    else for (auto& x : i.list(a[0])) p.push_back(i.scalar(x));
    return v_str(score::layout::clefFor(p));
}

inline void add_score(Interp& i) {
    OpaqueRecord sr;
    sr.get = [](Interp& in, const vptr& v, const vptr& k, bool& f) { return scoreGet(in, scoreOf(in, v), k, f); };
    sr.put = [](Interp& in, const vptr& v, const vptr& k, const vptr& x) { scorePut(in, scoreOf(in, v), k, x); };
    sr.pairs = [](Interp& in, const vptr& v) { return scorePairs(in, scoreOf(in, v)); };
    i.records["score"] = sr;
    OpaqueRecord er;
    er.get = [](Interp& in, const vptr& v, const vptr& k, bool& f) { return eventGet(in, eventOf(in, v), k, f); };
    er.put = [](Interp& in, const vptr& v, const vptr& k, const vptr& x) { eventPut(in, eventOf(in, v), k, x); };
    er.pairs = [](Interp& in, const vptr& v) { return eventPairs(in, eventOf(in, v)); };
    i.records["event"] = er;
    i.def("score-make", mus_score_make, 2, 2); i.def("score?", mus_scorep, 1, 1); i.def("score-add!", mus_score_add, 2, 2);
    i.def("score-take-id!", mus_score_take_id, 1, 1); i.def("score-duration", mus_score_duration, 1, 1);
    i.def("score-find", mus_score_find, 2, 2); i.def("score-remove-id!", mus_score_remove_id, 2, 2);
    i.def("score-query", mus_score_query, 3, 3); i.def("score-copy", mus_score_copy, 1, 1);
    i.def("score-flatten", mus_score_flatten, 2, 2); i.def("score-placed", mus_score_placed, 5, 5);
    i.def("score-read", mus_score_read, 1, 1); i.def("score-write", mus_score_write, 2, 2);
    i.def("score->text", mus_score_text, 1, 1); i.def("text->score", mus_text_score, 1, 1);
    i.def("score->data", mus_score_data, 1, 1); i.def("data->score", mus_data_score, 1, 1);
    i.def("score->coll", mus_score_coll, 1, 1); i.def("coll->score", mus_coll_score, 3, 3);
    i.def("score-pan-gains", mus_score_pan_gains, 2, 2);
    i.def("score-bbt", mus_score_bbt, 2, 2); i.def("score-bbt->sec", mus_score_bbt_sec, 4, 4);
    i.def("score-transpose!", mus_score_transpose, 2, 3); i.def("score-invert!", mus_score_invert, 2, 3);
    i.def("score-retrograde!", mus_score_retrograde, 1, 2); i.def("score-augment!", mus_score_augment, 2, 3);
    i.def("score-diminish!", mus_score_diminish, 2, 3); i.def("score-quantize-grid!", mus_score_quantize_grid, 3, 4);
    i.def("score-humanize!", mus_score_humanize, 4, 5); i.def("score-thin!", mus_score_thin, 3, 4);
    i.def("score-density!", mus_score_density, 3, 4); i.def("score-shift!", mus_score_shift, 2, 3);
    i.def("score-scale!", mus_score_scale, 2, 2);
    i.def("instrument-rank", mus_instrument_rank, 1, 1); i.def("score-orchestral-order", mus_orchestral_order, 0, 0);
    i.def("staff-clef", mus_staff_clef, 1, 1);
}

}  // namespace score_musil
using score_musil::add_score;
}  // namespace musil
