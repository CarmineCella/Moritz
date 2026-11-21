// granular_musil.h — the granulator and the morphological orchestration (granular.h) as Musil functions: music.mu's
// orchestrate-granular and orchestrate-morphological call granular-run; the envelopes' rules (env-at, draw-in,
// pick-from...), the target's analysis and the morphological measures are these builtins. Included by music.h.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "granular.h"

#ifdef OPAQUE      // Windows (wingdi.h) defines it; Musil has a value type of that name
#undef OPAQUE
#endif

namespace musil {
namespace granular_musil {

using namespace granular;

// Musil's values for the shared rules of granular.h (a scalar, a list)
struct value_traits {
    static bool is_num(const vptr& v) { return v && v->t == Value::NUM && v->num.size() == 1; }
    static double num(const vptr& v) { return v->num[0]; }
    static bool is_list(const vptr& v) { return v && v->t == Value::LIST; }
    static size_t size(const vptr& v) { return v->l.size(); }
    static const vptr& at(const vptr& v, size_t k) { return v->l[k]; }
    static vptr number(double d) { return v_num(d); }
    static vptr range(double a, double b) { return v_list({ v_num(a), v_num(b) }); }
};
inline pval pval_of(const vptr& v) {
    if (!v || v->t == Value::NIL) return pval::none();
    if (v->t == Value::NUM) {
        if (v->num.size() == 1) return pval::number(v->num[0]);
        pval l = pval::of({}); for (double d : v->num) l.list.push_back(pval::number(d)); return l;
    }
    if (v->t == Value::STR || v->t == Value::SYM) return pval::text(v->s);
    if (v->t == Value::LIST) { pval l = pval::of({}); for (auto& x : v->l) l.list.push_back(pval_of(x)); return l; }
    return pval::text(str_of(v));
}
inline vptr value_of(const pval& p) {
    switch (p.k) {
        case pval::NIL: return v_nil();
        case pval::NUM: return v_num(p.num);
        case pval::NAME: return v_sym(p.name);
        default: { vlist l; for (auto& x : p.list) l.push_back(value_of(x)); return v_list(std::move(l)); }
    }
}
inline vptr pair_of(const char* key, vptr v) { return v_list({ v_sym(key), std::move(v) }); }
inline vptr vec_of(const std::vector<double>& v) { varr a(v.size()); for (size_t k = 0; k < v.size(); k++) a[k] = v[k]; return v_arr(std::move(a)); }
inline std::vector<double> doubles_of(Interp& i, const vptr& v) { const varr& a = i.num(v); return std::vector<double>(std::begin(a), std::end(a)); }
// a record's value set in place (a pair added when the key is new)
inline void rec_put(const vptr& rec, const char* key, vptr v) {
    for (auto& p : rec->l) if (p && p->t == Value::LIST && p->l.size() == 2 && p->l[0] && (p->l[0]->t == Value::SYM || p->l[0]->t == Value::STR) && p->l[0]->s == key) { p->l[1] = std::move(v); return; }
    rec->l.push_back(pair_of(key, std::move(v)));
}
// the parameters of a run (a record), the target and the Markov score left out (they are read apart)
inline params params_of(Interp& i, const vptr& rec) {
    params p;
    if (!rec || rec->t == Value::NIL) return p;
    if (rec->t != Value::LIST) i.bad("the parameters are a record");
    for (auto& kv : rec->l) {
        if (!kv || kv->t != Value::LIST || kv->l.size() != 2 || !kv->l[0]) continue;
        std::string k = str_of(kv->l[0]);
        if (k == "target" || k == "markov-score") continue;
        p.set(k, pval_of(kv->l[1]));
    }
    return p;
}

// --- a database's entries as the granulator reads them, kept on the database (its 'granular pair) ---
struct db_cache {
    std::vector<orchidea::entry> entries;
    std::vector<char> available;
    std::vector<vptr> records;                 // the Musil entries, for the notes
    dbindex ix;
    const Value* index_rec = nullptr;          // the 'index the availability was read from (db-index!)
};
inline std::shared_ptr<db_cache> db_cache_of(Interp& i, const vptr& db) {
    if (!db || db->t != Value::LIST) i.bad("not a database (db-load makes one)");
    vptr entries = rec_get(db, "entries"), index = rec_get(db, "index");
    if (!entries || entries->t != Value::LIST) i.bad("not a database (db-load makes one)");
    if (!index || index->t != Value::LIST) i.bad("the database has no index: (db-index! db) first");
    vptr cached = rec_get(db, "granular");
    if (cached && cached->t == Value::OPAQUE && cached->opaque_tag == "granular-db") {
        auto c = std::static_pointer_cast<db_cache>(cached->opaque);
        if (c->index_rec == index.get() && c->records.size() == entries->l.size()) return c;
    }
    auto c = std::make_shared<db_cache>();
    std::unordered_map<const Value*, size_t> place;
    for (size_t k = 0; k < entries->l.size(); k++) {
        const vptr& e = entries->l[k];
        orchidea::entry x; x.index = (int)k;
        auto field = [&](const char* key) { vptr v = rec_get(e, key); return v ? str_of(v) : std::string("N"); };
        x.instr = field("instr"); x.tech = field("tech"); x.pitch = field("pitch"); x.dyn = field("dyn"); x.other = field("other");
        if (vptr mv = rec_get(e, "midi")) if (mv->t == Value::NUM && mv->num.size() == 1) x.midi = (int)mv->num[0];
        if (vptr f = rec_get(e, "features")) if (f->t == Value::NUM) x.features.assign(std::begin(f->num), std::end(f->num));
        c->entries.push_back(std::move(x));
        c->records.push_back(e);
        place[e.get()] = k;
    }
    c->available.assign(c->entries.size(), 0);
    for (auto& kv : index->l) {                                  // instrument -> its slot: 'all the available pitched entries
        if (!kv || kv->t != Value::LIST || kv->l.size() != 2) continue;
        vptr all = rec_get(kv->l[1], "all");
        if (all && all->t == Value::LIST) for (auto& e : all->l) { auto it = place.find(e.get()); if (it != place.end()) c->available[it->second] = 1; }
    }
    c->ix = make_index(c->entries, c->available);
    c->index_rec = index.get();
    rec_put(db, "granular", v_opaque("granular-db", c));
    return c;
}
// a note of a run as music.mu's note makes it (and what the orchestrators add)
inline vptr note_value(const db_cache& c, const vptr& db, const note& n, bool morph) {
    const vptr& e = c.records[(size_t)n.entry];
    vptr file = rec_get(e, "file");
    vlist l = { pair_of("kind", v_sym("note")), pair_of("source", file ? file : v_str("")), pair_of("db", db), pair_of("entry", e),
                pair_of("instr", v_str(n.instr)), pair_of("pitch", v_str(n.pitch)), pair_of("midi", v_num(n.midi)), pair_of("dyn", v_str(n.dyn)),
                pair_of("tech", v_str(n.tech)), pair_of("shift", v_num(n.shift)), pair_of("cents", v_num(n.cents)), pair_of("label", v_str(n.label)) };
    if (morph) { l.push_back(pair_of("gain", v_num(n.gain))); l.push_back(pair_of("atom", vec_of(n.atom))); l.push_back(pair_of("player", v_num(n.player))); }
    else { l.push_back(pair_of("player", v_num(n.player))); l.push_back(pair_of("gain", v_num(n.gain))); }
    return v_list(std::move(l));
}
inline vptr report_value(const report& r) {
    vlist un, pl;
    for (auto& u : r.unplayables)
        un.push_back(v_list({ pair_of("at", v_num(u.at)), pair_of("styles", value_of(u.styles)), pair_of("register", value_of(u.reg)),
                              pair_of("dynamics", u.strict ? v_sym(u.dyn) : v_nil()) }));
    for (auto& p : r.players) {
        vlist ins; for (auto& s : p.instrs) ins.push_back(v_str(s));
        pl.push_back(v_list({ pair_of("instrs", v_list(std::move(ins))), pair_of("notes", v_num(p.notes)), pair_of("busy", v_num(p.busy)),
                              pair_of("seat", p.seat.empty() ? v_nil() : v_str(p.seat)) }));
    }
    return v_list({ pair_of("seconds", v_num(r.seconds)), pair_of("due", v_num(r.due)), pair_of("events", v_num(r.events)), pair_of("notes", v_num(r.notes)),
                    pair_of("skipped-busy", v_num(r.skipped_busy)), pair_of("skipped-unplayable", v_num(r.skipped_unplayable)),
                    pair_of("substituted", v_num(r.substituted)), pair_of("saturation", v_num(r.saturation)), pair_of("unplayable", v_list(std::move(un))),
                    pair_of("players", v_list(std::move(pl))) });
}
// the players of an orchestra (music.mu's orchestra makes the records), with their state when it carries on
inline std::vector<player> players_of(Interp& i, const vptr& orch, bool state) {
    if (!orch || orch->t != Value::LIST) i.bad("the orchestra is a list of players (orchestra makes one)");
    std::vector<player> out;
    for (auto& r : orch->l) {
        player p;
        vptr ins = rec_get(r, "instrs");
        if (!ins || ins->t != Value::LIST || ins->l.empty()) i.bad("a player without instruments (orchestra makes the players)");
        for (auto& s : ins->l) p.instrs.push_back(str_of(s));
        if (vptr g = rec_get(r, "group")) if (g->t == Value::NUM && g->num.size() == 1) p.group = (int)g->num[0];
        if (state) {
            auto num = [&](const char* k, double d) { vptr v = rec_get(r, k); return v && v->t == Value::NUM && v->num.size() == 1 ? v->num[0] : d; };
            p.busy_until = num("busy-until", 0); p.seat_since = num("seat-since", 0);
            if (vptr v = rec_get(r, "last-pitch")) if (v->t == Value::NUM && v->num.size() == 1) { p.has_last_pitch = true; p.last_pitch = v->num[0]; }
            if (vptr v = rec_get(r, "last-tech")) if (v->t != Value::NIL) { p.has_last_tech = true; p.last_tech = str_of(v); }
            if (vptr v = rec_get(r, "seat")) if (v->t != Value::NIL) { p.seated = true; p.seat = str_of(v); }
            if (vptr v = rec_get(r, "starts")) if (v->t == Value::NUM) p.starts.assign(std::begin(v->num), std::end(v->num));
            if (vptr v = rec_get(r, "ends")) if (v->t == Value::NUM) p.ends.assign(std::begin(v->num), std::end(v->num));
        }
        out.push_back(std::move(p));
    }
    return out;
}
inline void players_back(const vptr& orch, const std::vector<player>& ps) {
    for (size_t k = 0; k < ps.size() && k < orch->l.size(); k++) {
        const vptr& r = orch->l[k]; const player& p = ps[k];
        rec_put(r, "k", v_num((double)k));
        rec_put(r, "busy-until", v_num(p.busy_until));
        rec_put(r, "last-pitch", p.has_last_pitch ? v_num(p.last_pitch) : v_nil());
        rec_put(r, "last-tech", p.has_last_tech ? v_sym(p.last_tech) : v_nil());
        rec_put(r, "seat", p.seated ? v_str(p.seat) : v_nil());
        rec_put(r, "seat-since", v_num(p.seat_since));
        rec_put(r, "starts", vec_of(p.starts));
        rec_put(r, "ends", vec_of(p.ends));
    }
}
// a target record (target-analyse) as granular.h's target; the spectra only when asked
inline target target_of(Interp& i, const vptr& rec, bool spectra = true, bool fine = true) {
    if (!rec || rec->t != Value::LIST) i.bad("not a target (target-analyse makes one)");
    auto get = [&](const char* k) { vptr v = rec_get(rec, k); if (!v) i.bad(std::string("not a target (target-analyse makes one): no ") + k); return v; };
    auto frames = [&](const char* k) {
        morph::spectra s; vptr v = get(k);
        if (v->t != Value::LIST) i.bad(std::string("the target's ") + k + " are a list of spectra");
        for (auto& f : v->l) s.push_back(doubles_of(i, f));
        return s;
    };
    target g;
    g.sr = i.scalar(get("sr")); g.block = (int)i.scalar(get("block")); g.hop = (int)i.scalar(get("hop")); g.seconds = i.scalar(get("seconds"));
    if (!(g.sr > 0) || g.hop < 1 || g.block < 1) i.bad("the target's rate, block and hop must be positive");
    vptr fb = rec_get(rec, "fine-block"); g.fine_block = fb ? (int)i.scalar(fb) : 8192;
    g.times = doubles_of(i, get("times")); g.attacks = doubles_of(i, get("attacks")); g.density = doubles_of(i, get("density"));
    g.centroid = doubles_of(i, get("centroid")); g.spread = doubles_of(i, get("spread")); g.low = doubles_of(i, get("low"));
    g.loudness = doubles_of(i, get("loudness"));
    if (spectra) g.spectra = frames("spectra");
    else { vptr v = get("spectra"); g.spectra.resize(v->t == Value::LIST ? v->l.size() : 0); }
    if (fine) g.fine = frames("fine");
    return g;
}
inline vptr target_value(const target& g) {
    vlist sp, fi;
    for (auto& f : g.spectra) sp.push_back(vec_of(f));
    for (auto& f : g.fine) fi.push_back(vec_of(f));
    return v_list({ pair_of("sr", v_num(g.sr)), pair_of("block", v_num(g.block)), pair_of("hop", v_num(g.hop)), pair_of("seconds", v_num(g.seconds)),
                    pair_of("times", vec_of(g.times)), pair_of("spectra", v_list(std::move(sp))), pair_of("fine", v_list(std::move(fi))),
                    pair_of("fine-block", v_num(g.fine_block)), pair_of("attacks", vec_of(g.attacks)), pair_of("density", vec_of(g.density)),
                    pair_of("centroid", vec_of(g.centroid)), pair_of("spread", vec_of(g.spread)), pair_of("low", vec_of(g.low)),
                    pair_of("loudness", vec_of(g.loudness)) });
}
inline double opt_num(Interp& i, const vptr& rec, const char* key, double def) { vptr v = rec_get(rec, key); return v && v->t != Value::NIL ? i.scalar(v) : def; }
template <class F> inline vptr guarded(Interp& i, F&& f) {
    try { return f(); }
    catch (std::runtime_error& e) { i.err(e.what()); }
}

// (granular-run db orch secs params table)   the granulator (granular.h) over secs seconds: db with its index (db-index!),
//   orch the players (orchestra), params as orchestrate-granular takes them ('method 'target and 'target for a
//   morphological orchestration), table the Markov method's (markov-table) or nil; => (list fragment report). What
//   orchestrate-granular and orchestrate-morphological run; with 'continue the players' records carry the state
inline run_hooks musil_hooks(Interp& i, const params& p) {
    run_hooks hooks;
    hooks.progress = [&i](double) { i.yield_check(); return true; };
    if (p.opt("trace", pval::number(0)).truthy()) hooks.trace = [&i](const std::string& s) { *i.out << s << "\n" << std::flush; };
    return hooks;
}
inline vptr run_value(Interp& i, const db_cache& c, const vptr& db, const vptr& orch, const std::vector<player>& players, bool carry, const run_result& r, bool morph) {
    for (auto& m : r.messages) *i.out << m << "\n" << std::flush;
    if (carry) players_back(orch, players);
    vlist out;
    for (auto& n : r.notes) out.push_back(v_list({ v_num(n.at), v_num(n.dur), note_value(c, db, n, morph) }));
    return v_list({ v_list(std::move(out)), report_value(r.rep) });
}
inline vptr mus_granular_run(vlist& a, Interp& i) {
    auto c = db_cache_of(i, a[0]);
    double secs = i.scalar(a[2]);
    params p = params_of(i, a[3]);
    bool carry = p.opt("continue", pval::number(0)).truthy();
    std::vector<player> players = players_of(i, a[1], carry);
    target g; const target* tp = nullptr;
    if (p.opt("method", pval::text("random")).str() == "target") { g = target_of(i, rec_get(a[3], "target")); tp = &g; }
    markov_table table; const markov_table* tab = nullptr;
    if (a[4] && a[4]->t == Value::LIST) {
        for (auto& kv : a[4]->l) {
            if (!kv || kv->t != Value::LIST || kv->l.size() != 2) continue;
            std::vector<double> row; if (kv->l[1]->t == Value::LIST) for (auto& x : kv->l[1]->l) row.push_back(i.scalar(x)); else row = doubles_of(i, kv->l[1]);
            table.push_back({ i.scalar(kv->l[0]), row });
        }
        tab = &table;
    }
    run_result r;
    try { r = granulate(c->ix, players, secs, p, tp, tab, i.rng, musil_hooks(i, p)); }
    catch (std::runtime_error& e) { i.err(e.what()); }
    return run_value(i, *c, a[0], a[1], players, carry, r, tp != nullptr);
}
// (morphological-run db orch target params)   one realisation of a morphological orchestration (granular.h): the
//   granulator over the target's length, its envelopes read off the target (target-envelopes), the decision at every
//   event a pursuit against the target's spectrum; => (list fragment report). What orchestrate-morphological runs
inline vptr mus_morphological_run(vlist& a, Interp& i) {
    auto c = db_cache_of(i, a[0]);
    params p = params_of(i, a[3]);
    bool carry = p.opt("continue", pval::number(0)).truthy();
    std::vector<player> players = players_of(i, a[1], carry);
    target g = target_of(i, a[2]);
    run_result r;
    try { r = orchestrate_morphological(c->ix, players, g, p, i.rng, musil_hooks(i, p)); }
    catch (std::runtime_error& e) { i.err(e.what()); }
    return run_value(i, *c, a[0], a[1], players, carry, r, true);
}
// (target-analyse-with x sr block hop opts)   what a morphological orchestration reads off a sound x (a vector at sr),
//   computed once: the frame spectra (block as the database's features), the attacks (the peaks of the spectral flux
//   against its moving median) and their density in events a second, the register (the centroid in MIDI pitch, the
//   spread around it in octaves, the lowest strong partial) and the loudness in dB, each a curve sampled every hop;
//   => a record ('sr 'block 'hop 'seconds 'times 'spectra 'fine 'fine-block 'attacks 'density 'centroid 'spread 'low
//   'loudness); 'fine holds the spectra of a long window ('fine-block, 8192: 5 Hz bins at 44.1 kHz) from which the peaks
//   are read. opts: 'threshold (2: a flux peak counts above this times the local median; 1.5 counts the ripples of a
//   held sound too), 'window (1 s: the span the density is counted over), 'fine
inline vptr mus_target_analyse_with(vlist& a, Interp& i) {
    std::vector<double> x = doubles_of(i, a[0]);
    analysis_options o; o.threshold = opt_num(i, a[4], "threshold", 2); o.window = opt_num(i, a[4], "window", 1); o.fine = (int)opt_num(i, a[4], "fine", 8192);
    return guarded(i, [&] { return target_value(analyse(x, i.scalar(a[1]), (int)i.scalar(a[2]), (int)i.scalar(a[3]), o)); });
}
// (target-envelopes target opts)   the granulator's envelopes from a target's curves: 'density from the flux peaks a
//   second times 'density-scale (1: as the target; 2 twice as busy), at least 'min-density (0.5: a held sound is
//   re-attacked now and then), 'register from the centroid and the spread (centroid ± 'width times the spread, in
//   octaves, extended down to the lowest strong partial: a bass note lies under the centroid of its own spectrum),
//   'dynamics as a level from the loudness ('range dB); opts also 'step (seconds per breakpoint, 0.25)
inline vptr mus_target_envelopes(vlist& a, Interp& i) {
    target g = target_of(i, a[0], false, false);
    envelope_options o; o.step = opt_num(i, a[1], "step", 0.25); o.width = opt_num(i, a[1], "width", 1); o.min_density = opt_num(i, a[1], "min-density", 0.5);
    o.density_scale = opt_num(i, a[1], "density-scale", 1); o.range = opt_num(i, a[1], "range", 40);
    return guarded(i, [&] { envelopes e = target_envelopes(g, o);
        return v_list({ pair_of("density", value_of(e.density)), pair_of("register", value_of(e.reg)), pair_of("dynamics", value_of(e.dynamics)) }); });
}
// (target-frame target t)   the index of the target's frame at a time
inline vptr mus_target_frame(vlist& a, Interp& i) { target g = target_of(i, a[0], false, false); return v_num((double)target_frame(g, i.scalar(a[1]))); }
// (target-level target t range)   the loudness at t as a dynamics level 0..1: 1 at the target's loudest point, 0 at
//   range dB under it (40 by default)
inline vptr mus_target_level(vlist& a, Interp& i) { target g = target_of(i, a[0], false, false); return v_num(target_level(g, i.scalar(a[1]), i.scalar(a[2]))); }
// (target-peaks target t n)   the frequencies (Hz) of the n strongest spectral peaks of the target at t (the fine frames
//   over the next 0.1 s averaged), above 30 Hz and a twentieth of the strongest, each refined by a parabola: the pitches
//   the notes may take, as the partials of a sound are the pitches an orchestra can double it with; => a vector
inline vptr mus_target_peaks(vlist& a, Interp& i) {
    target g = target_of(i, a[0], false, true);
    long n = i.index(a[2]); if (n < 0) n = 0;
    return guarded(i, [&] { return vec_of(target_peaks(g, i.scalar(a[1]), (size_t)n)); });
}
// (frames-mean spectra k0 k1 n)   the frames k0..k1 of a list of spectra averaged, their first n bins
inline vptr mus_frames_mean(vlist& a, Interp& i) {
    if (a[0]->t != Value::LIST || a[0]->l.empty()) i.bad("spectra: a list of vectors");
    long k0 = i.index(a[1]), k1 = i.index(a[2]), n = i.index(a[3]), m = (long)a[0]->l.size();
    if (k0 < 0 || k1 < k0 || k1 >= m || n < 0) i.bad("frames k0..k1 inside the list");
    morph::spectra s; for (long k = k0; k <= k1; k++) s.push_back(doubles_of(i, a[0]->l[(size_t)k]));
    return guarded(i, [&] { return vec_of(frames_mean(s, 0, (size_t)(k1 - k0), (size_t)n)); });
}
// (atom-persistence target features t persist max-length)   how long a sound goes on being part of the target from t:
//   the frames ahead in which the target still holds it (the projection of the frame on the sound's spectrum, in the
//   linear magnitudes, stays at least persist of its peak within the first 0.1 s: 0.5 is 6 dB down), in seconds up to
//   max-length; the length of a note whose sound the target keeps, short for one it drops
inline vptr mus_atom_persistence(vlist& a, Interp& i) {
    target g = target_of(i, a[0], true, false);
    std::vector<double> f = doubles_of(i, a[1]);
    return guarded(i, [&] { return v_num(atom_persistence(g, f, i.scalar(a[2]), i.scalar(a[3]), i.scalar(a[4]))); });
}
// (entry-f0 e sr block)   the frequency a database entry actually sounds at: the peak of its average spectrum nearest
//   its nominal pitch (within a semitone, refined by a parabola); the nominal frequency when the spectrum has no peak
//   there. sr and block as the features were computed
inline vptr mus_entry_f0(vlist& a, Interp& i) {
    orchidea::entry e; vptr f = rec_get(a[0], "features"), m = rec_get(a[0], "midi");
    if (!f || f->t != Value::NUM || !m) i.bad("a database entry (with features and midi)");
    e.features.assign(std::begin(f->num), std::end(f->num)); e.midi = (int)i.scalar(m);
    if (e.features.size() < 3) i.bad("an entry with too few features");
    return v_num(entry_cache::entry_f0(e, i.scalar(a[1]), i.scalar(a[2])));
}
// (target-dictionary db peaks players lo hi dyn styles)   the sounds a pursuit may draw on: for every player the
//   available sounds of its instruments whose pitch is one of the target's peaks (frequencies; within 60 cents) and
//   within lo..hi (MIDI), with the dynamics dyn (any when the instrument has not got it) and one of the styles (all when
//   nil); => a list of (list player entry)
inline vptr mus_target_dictionary(vlist& a, Interp& i) {
    auto c = db_cache_of(i, a[0]);
    std::vector<player> ps = players_of(i, a[2], false);
    std::vector<int> free; for (size_t k = 0; k < ps.size(); k++) free.push_back((int)k);
    std::vector<std::string> styles; bool has = a[6] && a[6]->t != Value::NIL;
    if (has) { if (a[6]->t == Value::LIST) for (auto& s : a[6]->l) styles.push_back(str_of(s)); else styles.push_back(str_of(a[6])); }
    vlist out;
    for (auto& x : target_dictionary(c->ix, doubles_of(i, a[1]), free, ps, i.scalar(a[3]), i.scalar(a[4]), str_of(a[5]), has ? &styles : nullptr))
        out.push_back(v_list({ a[2]->l[(size_t)x.player], c->records[(size_t)x.entry] }));
    return v_list(std::move(out));
}
// (granular-pitches db instr tech dyn)   the MIDI pitches an instrument was recorded at (on disk) with a technique (nil:
//   any) at a dynamics (nil: any), sorted; the database's index (db-index!) first: db-pitches-at
inline vptr mus_granular_pitches(vlist& a, Interp& i) {
    auto c = db_cache_of(i, a[0]);
    pitch_cache pc(c->ix);
    std::string tech = a[2] && a[2]->t != Value::NIL ? str_of(a[2]) : "", dyn = a[3] && a[3]->t != Value::NIL ? str_of(a[3]) : "";
    vlist out;
    for (int m : pc.pitches(str_of(a[1]), tech.empty() && !(a[2] && a[2]->t != Value::NIL) ? nullptr : &tech, a[3] && a[3]->t != Value::NIL ? &dyn : nullptr)) out.push_back(v_num(m));
    return v_list(std::move(out));
}
// (technique-short? t)   does a technique name belong to those short by nature (pizz, stac, secco, col-legno-batt,
//   slap, key, bartok, snap, tongue-ram, sforz, attack)?
inline vptr mus_technique_short(vlist& a, Interp&) { return v_bool(technique_short(str_of(a[0]))); }
// (moving-median v n)   each value replaced by the median of the n around it
inline vptr mus_moving_median(vlist& a, Interp& i) { return vec_of(morph::moving_median(doubles_of(i, a[0]), i.index(a[1]))); }
// (env-at e t)   the value of an env at time t: an interpolated number or range, or a set chosen by the crossfade
inline vptr mus_env_at(vlist& a, Interp& i) { return guarded(i, [&] { return env_at_t<value_traits>(a[0], i.scalar(a[1]), i.rng); }); }
// (value-at v t)   an env at a time, or the value itself
inline vptr mus_value_at(vlist& a, Interp& i) { return guarded(i, [&] { return value_at_t<value_traits>(a[0], i.scalar(a[1]), i.rng); }); }
// (draw-in range)   a random number in a range (a list: between its first and its last), or the value itself
inline vptr mus_draw_in(vlist& a, Interp& i) { if (!value_traits::is_list(a[0])) return a[0]; return guarded(i, [&] { return v_num(draw_range_t<value_traits>(a[0], i.rng)); }); }
// (pick-from set)   a random element of a set (or the value itself when it is not a list of alternatives)
inline vptr mus_pick_from(vlist& a, Interp& i) { return guarded(i, [&] { return pick_from_t<value_traits>(a[0], i.rng); }); }
// (numeric-range? v)   is v a list of two numbers?
inline vptr mus_numeric_range(vlist& a, Interp&) { return v_bool(numeric_range_t<value_traits>(a[0])); }
// (draw-duration range law)   a duration drawn in a range by a law: 'uniform, 'log (log-uniform), or an exponent
inline vptr mus_draw_duration(vlist& a, Interp& i) {
    if (!value_traits::is_list(a[0])) return a[0];
    if (a[0]->l.empty()) i.bad("an empty range");
    double lo = i.scalar(a[0]->l.front()), hi = i.scalar(a[0]->l.back());
    bool log = (a[1]->t == Value::SYM || a[1]->t == Value::STR) && a[1]->s == "log", ex = value_traits::is_num(a[1]);
    return v_num(draw_duration_in(lo, hi, log ? 1 : ex ? 2 : 0, ex ? a[1]->num[0] : 0, i.rng));
}
// (band-weight lo hi weight tilt m)   the weight of a pitch m in the band lo..hi (MIDI) under 'weight (1: the middle
//   more often, -1 the edges) and 'tilt (1: the top, -1 the bottom), what band-weigher's function gives
inline vptr mus_band_weight(vlist& a, Interp& i) { return v_num(band_weight(i.scalar(a[0]), i.scalar(a[1]), i.scalar(a[2]), i.scalar(a[3]), i.scalar(a[4]))); }
// (level->dynamics x)   a level 0..1 as the nearest label of ppp pp p mp mf f ff fff
inline vptr mus_level_dynamics(vlist& a, Interp& i) { return v_sym(level_dynamics(i.scalar(a[0]))); }
// (dynamics->level d)   a label of ppp .. fff as a level 0..1 (0.5 for one it does not know)
inline vptr mus_dynamics_level(vlist& a, Interp&) { return v_num(dynamics_level(str_of(a[0]))); }
// (level->gain x)   a level as a gain, 0.25 at ppp to 1 at fff (the sample carries the timbre of its dynamics, the gain
//   the finer steps between the recorded ones)
inline vptr mus_level_gain(vlist& a, Interp& i) { return v_num(level_gain(i.scalar(a[0]))); }
// (instrument-family instr)   'winds 'brass 'percussion 'keyboard 'plucked or 'strings by the instrument code (the
//   orchestral order); 'other for a code it does not know. What a 'balance may be given by
inline vptr mus_instrument_family(vlist& a, Interp&) { return v_sym(instrument_family(str_of(a[0]))); }
// (balance-gain balance instr)   the gain of an instrument under a 'balance: a record from instrument codes or families
//   to decibels, (record (list 'brass -6 'Hn -3)): the instrument's own entry, else its family's, else 0 dB; => a gain
inline vptr mus_balance_gain(vlist& a, Interp& i) { return guarded(i, [&] { return v_num(balance_gain(pval_of(a[0]), str_of(a[1]))); }); }
// (harmonic-series fundamental n)   the first n partials of a fundamental (a pitch) as MIDI pitches, rounded
inline vptr mus_harmonic_series(vlist& a, Interp& i) {
    long n = i.index(a[1]); if (n > 100000) i.bad("too many partials");
    vlist out; for (double m : harmonic_series(pitch_number(pval_of(a[0])), (int)n)) out.push_back(v_num(m));
    return v_list(std::move(out));
}
// (just-cents fundamental pitch n)   the cents a tempered pitch is away from the nearest of the first n partials of a
//   fundamental (a just overtone chord: the 7th partial -31, the 11th -49, the 13th +41, the 14th -31 cents); nil when
//   the pitch is not one of them (within a quarter tone)
inline vptr mus_just_cents(vlist& a, Interp& i) {
    double c; if (just_cents(pitch_number(pval_of(a[0])), pitch_number(pval_of(a[1])), (int)i.scalar(a[2]), c)) return v_num(c);
    return v_nil();
}

// (markov-learn lines)   the Markov method's table from melodic lines (lists of MIDI pitches in time order, one per
//   instrument): a record from each pitch to the pitches that followed it (repeated followers weigh more); markov-table
//   learns a score's lines so, Moritz's orchidea a line of pitches
inline vptr mus_markov_learn(vlist& a, Interp& i) {
    if (a[0]->t != Value::LIST) i.bad("a list of lines (lists of MIDI pitches)");
    std::vector<std::vector<double>> lines;
    for (auto& l : a[0]->l) {
        std::vector<double> line;
        if (l->t == Value::LIST) for (auto& x : l->l) line.push_back(i.scalar(x)); else line = doubles_of(i, l);
        lines.push_back(std::move(line));
    }
    vlist out;
    for (auto& r : markov_learn(lines)) { vlist f; for (double x : r.second) f.push_back(v_num(x)); out.push_back(v_list({ v_num(r.first), v_list(std::move(f)) })); }
    return v_list(std::move(out));
}

inline void add_granular(Interp& i) {
    i.def("granular-run", mus_granular_run, 5, 5); i.def("morphological-run", mus_morphological_run, 4, 4); i.def("target-analyse-with", mus_target_analyse_with, 5, 5);
    i.def("target-envelopes", mus_target_envelopes, 2, 2); i.def("target-frame", mus_target_frame, 2, 2); i.def("target-level", mus_target_level, 3, 3);
    i.def("target-peaks", mus_target_peaks, 3, 3); i.def("frames-mean", mus_frames_mean, 4, 4); i.def("atom-persistence", mus_atom_persistence, 5, 5);
    i.def("entry-f0", mus_entry_f0, 3, 3); i.def("target-dictionary", mus_target_dictionary, 7, 7); i.def("granular-pitches", mus_granular_pitches, 4, 4);
    i.def("technique-short?", mus_technique_short, 1, 1); i.def("moving-median", mus_moving_median, 2, 2);
    i.def("env-at", mus_env_at, 2, 2); i.def("value-at", mus_value_at, 2, 2); i.def("draw-in", mus_draw_in, 1, 1); i.def("pick-from", mus_pick_from, 1, 1);
    i.def("numeric-range?", mus_numeric_range, 1, 1); i.def("draw-duration", mus_draw_duration, 2, 2); i.def("band-weight", mus_band_weight, 5, 5);
    i.def("level->dynamics", mus_level_dynamics, 1, 1); i.def("dynamics->level", mus_dynamics_level, 1, 1); i.def("level->gain", mus_level_gain, 1, 1);
    i.def("instrument-family", mus_instrument_family, 1, 1); i.def("balance-gain", mus_balance_gain, 2, 2);
    i.def("harmonic-series", mus_harmonic_series, 2, 2); i.def("markov-learn", mus_markov_learn, 1, 1); i.def("just-cents", mus_just_cents, 3, 3);
}

}  // namespace granular_musil
}  // namespace musil
