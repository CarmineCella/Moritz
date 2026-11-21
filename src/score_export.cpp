// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// score_export.cpp — a score as MusicXML (Musil's music.mu) or as a standard MIDI file (Musil's midi.h): score_export.h.

#include "score_export.h"

#include "fileio.h"
#include "musil_host.h"
#include "session.h"

#include "music/midi.h"
#include "music/score.h"
#include "music/score_musil.h"

#include <cmath>

namespace moritz {

namespace sc = musil::score;

bool parseScoreFormat(const std::string& w, ScoreFormat& f) {
    if (w == "musicxml" || w == "xml") { f = ScoreFormat::MusicXML; return true; }
    if (w == "midi" || w == "mid") { f = ScoreFormat::Midi; return true; }
    return false;
}

const char* scoreFormatExtension(ScoreFormat f) { return f == ScoreFormat::Midi ? ".mid" : ".musicxml"; }

namespace {

// a copy of s with the scores placed in it brought to the top (their notes and sounds at their place)
std::shared_ptr<sc::Score> flatCopy(const sc::Score& s) {
    auto c = std::make_shared<sc::Score>(s.name, s.sr);
    c->bpm = s.bpm;
    c->meterNum = s.meterNum;
    c->meterDen = s.meterDen;
    c->rows = s.rows;
    c->extra = s.extra;
    c->path = s.path;
    std::vector<sc::Placed> flat;
    sc::flatten(s, flat);
    std::vector<std::shared_ptr<sc::Event>> evs;
    evs.reserve(flat.size());
    for (auto& p : flat) {
        auto e = std::make_shared<sc::Event>(p.ev);
        if (p.depth > 0) e->id = 0;              // a fresh id (the top's own are kept)
        e->host.reset();
        evs.push_back(std::move(e));
    }
    c->assign(std::move(evs));
    return c;
}

// what score-notate made, as the Console says it
struct XmlResult {
    std::string text;
    int notes = 0, parts = 0, bars = 0;
    std::string error;
};

}  // namespace

int microtonalNotes(const sc::Score& s) {
    std::vector<sc::Placed> flat;
    try { sc::flatten(s, flat); } catch (const std::exception&) { return 0; }
    int n = 0;
    for (auto& p : flat)
        if (p.ev.kind == sc::Kind::Note && p.ev.variant != "chord") {
            double x = p.ev.sounding();
            if (std::isfinite(x) && std::fabs(x - std::round(x)) > 0.005) ++n;
        }
    return n;
}

void exportScore(const sc::Score& s0, ScoreFormat f, const std::string& path, const ScoreExportOptions& o,
                 std::function<void(const ScoreExported&)> done0) {
    auto done = [done0](bool ok, const std::string& m, int notes = 0) { done0(ScoreExported{ok, m, notes}); };
    std::shared_ptr<sc::Score> s;
    try { s = flatCopy(s0); } catch (const std::exception& e) { done(false, e.what()); return; }
    std::string file = fileio::absolute(path);
    if (f == ScoreFormat::Midi) {
        musil::midi_write_options mo;
        mo.bend = o.bend;
        musil::midi_written w;
        try { w = musil::score_midi(*s, mo); } catch (const std::exception& e) { done(false, e.what()); return; }
        std::string err;
        if (!fileio::writeAtomic(file, w.bytes, &err)) { done(false, err); return; }
        std::string m = "exported " + std::to_string(w.notes) + " notes in " + std::to_string(w.parts.size()) +
                        (w.parts.size() == 1 ? " part" : " parts") + " as MIDI";
        if (w.rounded) m += " (" + std::to_string(w.rounded) + " rounded to the semitone)";
        done(true, m + ": " + file, (int)w.notes);
        return;
    }
    // MusicXML: music.mu's notation, on an interpreter of its own (a code box's permissions: it only computes)
    ContextHooks h;
    h.restricted = true;
    h.preload = {"music.mu"};
    auto ctx = std::make_shared<LangContext>("export", h);
    auto result = std::make_shared<XmlResult>();
    auto then = [s, result, file](musil::Interp& I) {
        I.def("export-score", musil::score_musil::v_score(s));
        musil::vptr r = I.run(
            "var m (score-notate export-score (list))\n"
            "notation-report m\n"
            "(list (musicxml-text m) (sum-by (get m 'parts) (function (p) (length (get p 'notes)))) (length (get m 'parts)) (get m 'bars))\n",
            "export");
        if (!r || r->t != musil::Value::LIST || r->l.size() != 4 || r->l[0]->t != musil::Value::STR)
            I.err("export: score-notate gave no notation");
        result->text = r->l[0]->s;
        result->notes = (int)I.scalar(r->l[1]);
        result->parts = (int)I.scalar(r->l[2]);
        result->bars = (int)I.scalar(r->l[3]);
        if (!fileio::writeAtomic(file, result->text, &result->error) && result->error.empty()) result->error = "can't write " + file;
    };
    bool started = ctx->eval("", "export", 1, [ctx, result, file, done](const EvalResult& r) {
        if (!r.ok) { done(false, "MusicXML: " + r.error); return; }
        if (!result->error.empty()) { done(false, result->error); return; }
        done(true, "exported " + std::to_string(result->notes) + " notes in " + std::to_string(result->parts) +
                       (result->parts == 1 ? " part, " : " parts, ") + std::to_string(result->bars) +
                       (result->bars == 1 ? " bar" : " bars") + " as MusicXML: " + file, result->notes);
    }, then);
    if (!started) done(false, "MusicXML: the exporter is busy");
}

}  // namespace moritz
