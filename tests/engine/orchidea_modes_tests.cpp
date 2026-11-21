// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// orchidea_modes_tests.cpp — orchidea's granular and morphological modes (objects_orchidea.cpp on Musil's
// music/granular.h): the notes, the envelopes and a buffer~'s curve, the same seed the same notes, the realisations,
// two sections carrying an orchestra's bookings on, the morphological mode on a target, the score out; bad inputs and
// empty spaces answered with a message; a run in the background stopped and a box deleted while it runs (the
// orchestra's players released)

#include "musil_host.h"
#include "transport.h"

#include "music/score_layout.h"
#include "rig.h"

#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <thread>

using namespace moritz;
using etest::Rig;

namespace {

class Catch : public Box {
public:
    Catch() { setPorts({Port::Control}, {}); cls = "catch"; }
    void message(int, const Message& m) override { got.push_back(m); }
    std::vector<Message> got;
};

const char* kDb = "data/microsol/microsol.spectrum.db";

// an orchestra on MicroSOL and two orchideas (a, b) sharing it, their outlets caught
struct ModeRig {
    Rig r;
    Catch *done[2], *notes[2], *sat[2];
    explicit ModeRig(const std::string& extra = "")
        : r("(patch (obj t buffer~ tgt data/archeos-bell.wav) (obj o buffer~ mix) (obj or orchestra band " + std::string(kDb) + ")"
            " (obj a orchidea band) (obj b orchidea band)" + extra + ")") {
        const char* ids[2] = {"a", "b"};
        for (int k = 0; k < 2; ++k) {
            auto d = std::make_unique<Catch>(), n = std::make_unique<Catch>(), x = std::make_unique<Catch>();
            done[k] = d.get(); notes[k] = n.get(); sat[k] = x.get();
            Box* bd = r.p.addBox(std::move(d), 0, 300 + 50 * k);
            Box* bn = r.p.addBox(std::move(n), 100, 300 + 50 * k);
            Box* bx = r.p.addBox(std::move(x), 200, 300 + 50 * k);
            r.p.connect(r.box(ids[k]), 1, bd, 0);
            r.p.connect(r.box(ids[k]), 2, bn, 0);
            r.p.connect(r.box(ids[k]), 3, bx, 0);
        }
    }
    void send(int k, const std::string& text) {
        for (auto& part : splitMessages(text)) r.box(k ? "b" : "a")->message(0, part);
    }
    void orchestra(const std::string& text) { r.box("or")->message(0, parseMessage(text)); }
    void clear() { for (int k = 0; k < 2; ++k) { done[k]->got.clear(); notes[k]->got.clear(); sat[k]->got.clear(); } r.printed.clear(); }
    static std::vector<Message> splitMessages(const std::string& text) {
        std::vector<Message> out;
        size_t from = 0;
        while (from <= text.size()) {
            size_t c = text.find(',', from);
            std::string one = text.substr(from, c == std::string::npos ? std::string::npos : c - from);
            Message m = parseMessage(one);
            if (!m.empty()) out.push_back(m);
            if (c == std::string::npos) break;
            from = c + 1;
        }
        return out;
    }
};

std::string notesText(const std::vector<Message>& got) {
    std::string s;
    for (auto& m : got) s += messageToString(m) + "\n";
    return s;
}
double levelOf(const std::string& d) {
    static const char* s[] = {"ppp", "pp", "p", "mp", "mf", "f", "ff", "fff"};
    for (int k = 0; k < 8; ++k) if (d == s[k]) return k;
    return -1;
}
// no player plays two notes at once: the notes (onset, duration in ms; player) of every list given
bool nobodyTwice(const std::vector<const std::vector<Message>*>& lists) {
    std::map<int, std::vector<std::pair<double, double>>> by;
    for (auto* l : lists)
        for (auto& m : *l) by[(int)m[9].f].push_back({m[5].f, m[5].f + m[6].f});
    for (auto& kv : by) {
        auto v = kv.second;
        std::sort(v.begin(), v.end());
        for (size_t k = 1; k < v.size(); ++k) if (v[k].first < v[k - 1].second - 0.5) return false;
    }
    return true;
}

}  // namespace

// the granular mode: notes a player can play (an instrument of the orchestra, a pitch of the register), a stereo mix,
// the saturation; the same seed the same notes; two realisations, the second chosen
TEST(orchidea_granular_writes_notes) {
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    ModeRig o;
    o.orchestra("players Ob Hn Vn|Vc Vn+Vc");
    o.send(0, "mode granular, seed 8, solutions 2, density 4 6, register 3 4, run 6 mix");
    CHECK(o.done[0]->got.size() == 1);
    CHECK(o.notes[0]->got.size() > 10);
    CHECK(o.sat[0]->got.size() == 1 && o.sat[0]->got[0][0].f >= 0.f && o.sat[0]->got[0][0].f <= 1.f);
    bool fits = true;
    for (auto& m : o.notes[0]->got) {
        std::string i = m[1].str();
        int p = musil::pitch_to_midi(m[2].str());
        fits = fits && m.size() == 10 && (i == "Ob" || i == "Hn" || i == "Vn" || i == "Vc") && p >= 48 && p <= 71 && m[5].f >= 0 && m[5].f < 6000 &&
               m[9].f >= 0 && m[9].f < 5;
    }
    CHECK(fits);
    BufferBox* mix = findBuffer(&o.r.p, "mix");
    CHECK(mix && mix->buf.channels == 2 && mix->buf.frames > 44100 * 6);
    std::string first = notesText(o.notes[0]->got);
    o.clear();
    o.send(0, "run 6");
    CHECK(notesText(o.notes[0]->got) == first);
    o.clear();
    o.send(0, "choose 1 2");
    std::string other = notesText(o.notes[0]->got);
    CHECK(!other.empty() && other != first);
    o.clear();
    o.send(0, "show");
    CHECK(o.r.printedSomething("2 realisations"));
    o.send(0, "report");
    CHECK(o.r.printedSomething("events due") && o.r.printedSomething("Vn|Vc:"));
    jobsSynchronous = sync;
}

// envelopes (at t value ...): the dynamics from pp to ff; a buffer~'s curve over the run (one channel a number, two a
// range); a parameter without a value back to its default; the markov method on a line of pitches
TEST(orchidea_granular_envelopes_and_curves) {
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    ModeRig o(" (obj c buffer~ curve) (obj rg buffer~ band2)");
    o.orchestra("players Vn Vc Ob Hn");
    o.send(0, "mode granular, seed 3, density 6, register 4 4, dynamics at 0 pp at 8 ff, run 8");
    auto& n = o.notes[0]->got;
    CHECK(n.size() > 10);
    if (n.size() > 10) {
        CHECK(n.front()[3].str() == "pp");
        CHECK(n.back()[3].str() == "ff");
    }
    // a level from a buffer~ (0 to 1 over the run): a crescendo from ppp to fff
    SampleBuffer s;
    s.frames = 101; s.channels = 1; s.sr = 44100;
    for (int k = 0; k <= 100; ++k) s.data.push_back(k / 100.f);
    findBuffer(&o.r.p, "curve")->replace(std::move(s));
    o.clear();
    o.send(0, "dynamics buffer curve, run 8");
    CHECK(n.size() > 10);
    if (n.size() > 10) CHECK(levelOf(n.front()[3].str()) <= 1 && levelOf(n.back()[3].str()) >= 6);
    // the register from a two-channel buffer~: octave 4 then 5 (where MicroSOL has the horn's E5 alone)
    SampleBuffer r2;
    r2.frames = 2; r2.channels = 2; r2.sr = 44100;
    r2.data = {4.f, 4.f, 5.f, 5.f};
    findBuffer(&o.r.p, "band2")->replace(std::move(r2));
    o.clear();
    o.send(0, "dynamics, register buffer band2, run 8");
    CHECK(n.size() > 10);
    if (n.size() > 10) {
        CHECK(musil::pitch_to_midi(n.front()[2].str()) < 72);
        CHECK(musil::pitch_to_midi(n.back()[2].str()) == 76 && n.back()[1].str() == "Hn");
        CHECK(n.front()[3].str() == "mf");                // the dynamics back to its default
    }
    // the markov method: a line of pitches learnt, the notes among them
    o.clear();
    o.send(0, "register 4 4, method markov, markov C4 D4 E4 C4, run 4");
    size_t among = 0;                                     // the horn has no E4: its nearest instead
    for (auto& m : n) { int p = musil::pitch_to_midi(m[2].str()); among += (p == 60 || p == 62 || p == 64) ? 1 : 0; }
    CHECK(!n.empty() && among >= n.size() * 7 / 10);
    jobsSynchronous = sync;
}

// two sections on one orchestra that carry its bookings on (continue 1): in any order of time, never a player twice;
// rest clears them; one run books the players at a time
TEST(orchidea_granular_sections_continue) {
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    ModeRig o(" (obj s score sections 1) (obj mg message \"merge $2\") (connect a 4 s 0) (connect b 4 mg 0) (connect mg 0 s 0)");
    o.orchestra("players Ob Hn Vn Vc");
    o.send(0, "mode granular, seed 4, continue 1, start 3, density 4, duration 2, register 4 4, run 4");
    o.send(1, "mode granular, seed 4, continue 1, start 0, density 4, duration 2, register 4 4, run 6");
    CHECK(!o.notes[0]->got.empty() && !o.notes[1]->got.empty());
    CHECK(nobodyTwice({&o.notes[0]->got, &o.notes[1]->got}));
    auto both = scoreOfBox(o.r.box("s"));            // the second section merged into the first's score: one piece
    CHECK(both && both->size() == o.notes[0]->got.size() + o.notes[1]->got.size());
    bool rowsOrdered = both && !both->rows.empty();
    if (both) for (size_t k = 1; k < both->rows.size(); ++k)
        rowsOrdered = rowsOrdered && musil::score::layout::instrumentRank(both->rows[k - 1].name) < musil::score::layout::instrumentRank(both->rows[k].name);
    CHECK(rowsOrdered);
    bool placed = true;
    for (auto& m : o.notes[0]->got) placed = placed && m[5].f >= 3000;
    CHECK(placed);                                         // the first section's notes from 3 s
    // without continue the same sections overbook
    std::string carried = notesText(o.notes[1]->got);
    o.orchestra("rest");
    o.clear();
    o.send(0, "continue 0, run 4");
    o.send(1, "continue 0, run 6");
    CHECK(!nobodyTwice({&o.notes[0]->got, &o.notes[1]->got}));
    // rest: the second section alone again, as if first
    o.orchestra("rest");
    o.clear();
    o.send(1, "continue 1, run 6");
    std::string alone = notesText(o.notes[1]->got);
    CHECK(alone != carried);
    o.orchestra("rest");
    o.clear();
    o.send(1, "run 6");
    CHECK(notesText(o.notes[1]->got) == alone);
    jobsSynchronous = sync;
}

// the morphological mode on a target: notes with durations and cents within a quarter tone; the same seed the same
// notes; the genetic decision; the score out with the dynamics as gains
TEST(orchidea_morphological_mode) {
    namespace sc = musil::score;
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    ModeRig o(" (obj s score morph 1) (connect a 4 s 0)");
    o.send(0, "mode morphological, seed 2, min-density 2, run tgt mix");
    auto& n = o.notes[0]->got;
    CHECK(o.done[0]->got.size() == 1);
    CHECK(!n.empty());
    bool ok = true;
    for (auto& m : n) ok = ok && m[6].f > 0 && std::fabs(m[8].f) <= 50 && m[5].f < 6300;
    CHECK(ok);
    std::string first = notesText(n);
    o.clear();
    o.send(0, "run tgt");
    CHECK(notesText(n) == first);
    auto s = scoreOfBox(o.r.box("s"));
    CHECK(s && s->size() == n.size());
    bool gains = s && s->size() > 0;
    if (s) for (auto* e : s->byTime()) gains = gains && e->gain > 0.2 && e->gain <= 1 && sc::field(e->extra, "sound");
    CHECK(gains);
    o.clear();
    o.send(0, "decision genetic, population 20, epochs 5, run tgt");
    CHECK(o.done[0]->got.size() == 1 && !n.empty());
    o.send(0, "report");
    CHECK(o.r.printedSomething("morphological:"));
    jobsSynchronous = sync;
}

// bad inputs and empty spaces: a message, never a crash
TEST(orchidea_modes_bad_inputs) {
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    ModeRig o;
    o.send(0, "mode sideways");
    CHECK(o.r.printedSomething("mode is mimetic, granular or morphological"));
    o.send(0, "mode granular, run tgt");
    CHECK(o.r.printedSomething("run seconds"));
    o.send(0, "run 0");
    o.send(0, "run 100000");
    CHECK(o.done[0]->got.empty());
    o.send(0, "density at 0");
    CHECK(o.r.printedSomething("at time value"));
    o.send(0, "balance brass");
    CHECK(o.r.printedSomething("balance: names and decibels"));
    o.clear();
    o.send(0, "register high, run 2");
    CHECK(o.r.printedSomething("register"));
    CHECK(o.done[0]->got.empty());
    o.clear();
    o.send(0, "register 1 1, run 2");                       // nothing of the orchestra down there: unplayable, no notes
    CHECK(o.r.printedSomething("unplayable"));
    CHECK(o.done[0]->got.size() == 1 && o.notes[0]->got.empty());
    o.clear();
    o.send(0, "register 4 4, density buffer nothere, run 2");
    CHECK(o.r.printedSomething("no curve in buffer~ nothere"));
    o.clear();
    o.send(0, "density, styles nonesuch, run 2");          // a style nobody has
    CHECK(o.done[0]->got.size() == 1 && o.notes[0]->got.empty());
    o.clear();
    o.send(0, "styles, density -5, register -100 100, run 2");   // absurd values: brought into range or refused
    o.clear();
    o.send(0, "register 4 4, choose 2 1");
    CHECK(o.r.printedSomething("one segment"));
    o.send(0, "choose 1 9");
    CHECK(o.r.printedSomething("realisations"));
    // the morphological mode: no target, a silent one, a few samples
    o.clear();
    o.send(1, "mode morphological, run nothere");
    CHECK(o.r.printedSomething("no sound in buffer~ nothere"));
    for (long frames : {44100L, 10L}) {
        SampleBuffer s;
        s.frames = frames; s.sr = 44100;
        s.data.assign((size_t)frames, 0.f);
        findBuffer(&o.r.p, "tgt")->replace(std::move(s));
        o.clear();
        o.send(1, "run tgt");
        CHECK(o.done[1]->got.size() == 1 && o.notes[1]->got.empty());
    }
    // an orchestra none of whose instruments the database has
    o.orchestra("players Tba");
    o.clear();
    o.send(0, "register 4 4, run 2");
    CHECK(o.notes[0]->got.empty());
    jobsSynchronous = sync;
}

// in the background: a long run stopped (no notes, no done; the orchestra's players released for the next run that
// continues); a run that continues refused while another books the players; a box deleted while it runs
TEST(orchidea_granular_in_the_background) {
    bool sync = jobsSynchronous;
    jobsSynchronous = false;
    auto drainUntil = [](const std::function<bool()>& cond) {
        for (int k = 0; k < 6000 && !cond(); ++k) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            drainMain();
        }
    };
    {
        ModeRig o;
        o.send(0, "mode granular, continue 1, density 400, duration 0.01, polyphony 1000, run 3600");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        drainMain();
        o.send(1, "mode granular, continue 1, run 2");
        drainUntil([&] { return o.r.printedSomething("another run is booking"); });
        CHECK(o.r.printedSomething("another run is booking"));
        o.send(0, "stop");
        for (int k = 0; k < 20; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); drainMain(); }
        CHECK(o.done[0]->got.empty() && o.notes[0]->got.empty());
        o.send(1, "run 2");
        drainUntil([&] { return !o.done[1]->got.empty(); });
        CHECK(o.done[1]->got.size() == 1 && !o.notes[1]->got.empty());
        // deleted while running: the players released
        o.clear();
        o.send(0, "run 3600");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto t0 = std::chrono::steady_clock::now();
        o.r.p.removeBoxes({o.r.box("a")});
        CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 5);
        for (int k = 0; k < 20; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); drainMain(); }
        o.send(1, "run 2");
        drainUntil([&] { return !o.done[1]->got.empty(); });
        CHECK(o.done[1]->got.size() == 1);
        CHECK(!o.r.printedSomething("another run is booking"));
    }
    {   // the morphological mode stopped while it analyses or searches
        ModeRig o;
        o.send(0, "mode morphological, decision genetic, population 2000, epochs 2000, patience 2000, run tgt");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        drainMain();
        o.send(0, "stop");
        for (int k = 0; k < 20; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); drainMain(); }
        CHECK(o.done[0]->got.empty());
    }
    jobsSynchronous = sync;
}
