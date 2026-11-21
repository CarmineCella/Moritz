// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// orchidea_tests.cpp — orchestra and orchidea (objects_orchidea.cpp): a target orchestrated on MicroSOL, the notes and
// the mix; the same seed the same notes; the players, ranges and techniques obeyed; the job cancelled, its box deleted
// while it runs, a run waiting for its orchestra's database; bad inputs answered with a message, never a crash

#include "musil_host.h"
#include "roll_edit.h"
#include "score_doc.h"
#include "transport.h"

#include "music/score_layout.h"
#include "music/score_solutions.h"
#include "music/score_text.h"
#include "rig.h"

#include <tuple>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

using namespace moritz;
using etest::Rig;

namespace {

// what comes out of an outlet: every message
class Catch : public Box {
public:
    Catch() { setPorts({Port::Control}, {}); cls = "catch"; }
    void message(int, const Message& m) override { got.push_back(m); }
    std::vector<Message> got;
};

std::string tempPath(const std::string& name) {
    const char* t = std::getenv("TMPDIR");
    std::string d = t && *t ? t : "/tmp";
    if (d.back() != '/') d += '/';
    return d + name;
}

const char* kDb = "data/microsol/microsol.spectrum.db";

// a patch with a target, an output buffer~, an orchestra on MicroSOL and an orchidea, its outlets caught
struct OrchRig {
    Rig r;
    Catch *done, *notes, *dist;
    explicit OrchRig(const std::string& extra = "", const std::string& target = "data/archeos-bell.wav")
        : r("(patch (obj t buffer~ tgt " + target + ") (obj o buffer~ mix) (obj or orchestra band " + kDb + ")"
            " (obj oi orchidea band)" + extra + ")") {
        auto d = std::make_unique<Catch>(), n = std::make_unique<Catch>(), x = std::make_unique<Catch>();
        done = d.get(); notes = n.get(); dist = x.get();
        Box* a = r.p.addBox(std::move(d), 0, 300);
        Box* b = r.p.addBox(std::move(n), 100, 300);
        Box* c = r.p.addBox(std::move(x), 200, 300);
        r.p.connect(r.box("oi"), 1, a, 0);
        r.p.connect(r.box("oi"), 2, b, 0);
        r.p.connect(r.box("oi"), 3, c, 0);
    }
    void send(const Message& m) { r.box("oi")->message(0, m); }
    void orchestra(const Message& m) { r.box("or")->message(0, m); }
    void clear() { done->got.clear(); notes->got.clear(); dist->got.clear(); r.printed.clear(); }
};

// the notes as text, to compare runs
std::string notesText(const std::vector<Message>& got) {
    std::string s;
    for (auto& m : got) s += messageToString(m) + "\n";
    return s;
}

}  // namespace

// a static target on MicroSOL: notes (k instr pitch dyn tech onset dur path cents player), a stereo mix as long as the
// target plus 0.6 s, the distance, the done bang
TEST(orchidea_orchestrates_a_target) {
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    OrchRig o;
    o.send({Atom("partials"), Atom(0.f)});
    o.send({Atom("run"), Atom("tgt"), Atom("mix")});
    jobsSynchronous = sync;
    CHECK(o.done->got.size() == 1);
    CHECK(!o.notes->got.empty());
    CHECK(o.dist->got.size() == 1 && o.dist->got[0][0].f > 0.f);
    bool shaped = true, onDisk = true, inOrchestra = true;
    for (auto& m : o.notes->got) {
        shaped = shaped && m.size() == 10 && m[0].isFloat() && m[5].isFloat() && m[6].isFloat() && m[6].f > 6000.f;
        if (m.size() == 10) {
            std::ifstream f(m[7].str());
            onDisk = onDisk && (bool)f;
            std::string i = m[1].str();
            inOrchestra = inOrchestra && (i == "Ob" || i == "Hn" || i == "Vn" || i == "Vc");
        }
    }
    CHECK(shaped);
    CHECK(onDisk);
    CHECK(inOrchestra);
    BufferBox* mix = findBuffer(&o.r.p, "mix");
    CHECK(mix && mix->buf.channels == 2);
    CHECK(mix && std::abs(mix->buf.frames - (long)std::floor(6.193968 * 44100 + 0.6 * 44100)) < 2);
    float peak = 0;
    if (mix) for (float v : mix->buf.data) peak = std::max(peak, std::fabs(v));
    CHECK(peak > .01f && peak <= .981f);
    // the notes as a roll: a frame a note, onset, pitch, player, duration
    o.send({Atom("roll"), Atom("tgt")});
    BufferBox* roll = findBuffer(&o.r.p, "tgt");
    CHECK(roll && roll->buf.channels == 4 && roll->buf.frames == (long)o.notes->got.size());
    if (roll && roll->buf.frames > 0 && !o.notes->got.empty()) {
        const Message& n = o.notes->got[0];
        CHECK(std::fabs(roll->buf.at(0, 0) - n[5].f / 1000.f) < 1e-4f);
        CHECK(std::fabs(roll->buf.at(0, 1) - (musil::pitch_to_midi(n[2].str()) + n[8].f / 100.f)) < 1e-4f);
        CHECK(roll->buf.at(0, 2) == n[9].f);
    }
}

// the same seed gives the same notes; the notes come again on `notes`; another solution is chosen with `choose`
TEST(orchidea_same_seed_same_notes) {
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    OrchRig o;
    o.send({Atom("partials"), Atom(0.f)});
    o.send({Atom("seed"), Atom(7.f)});
    o.send({Atom("population"), Atom(60.f)});
    o.send({Atom("epochs"), Atom(40.f)});
    o.send({Atom("run"), Atom("tgt")});
    std::string first = notesText(o.notes->got);
    o.clear();
    o.send({Atom("run"), Atom("tgt")});
    std::string second = notesText(o.notes->got);
    o.clear();
    o.send({Atom("notes")});
    std::string again = notesText(o.notes->got);
    o.clear();
    o.send({Atom("choose"), Atom(1.f), Atom(2.f)});
    std::string other = notesText(o.notes->got);
    jobsSynchronous = sync;
    CHECK(!first.empty());
    CHECK(first == second);
    CHECK(first == again);
    CHECK(!other.empty() && other != first);
}

// the orchestra's players, ranges, techniques and dynamics are what the search may use
TEST(orchidea_obeys_the_orchestra) {
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    OrchRig o;
    o.send({Atom("partials"), Atom(0.f)});
    o.send({Atom("sparsity"), Atom(0.f)});
    o.orchestra({Atom("players"), Atom("Vn"), Atom(3.f), Atom("Vc")});
    o.orchestra({Atom("range"), Atom("Vn"), Atom("D4"), Atom(64.f)});
    o.send({Atom("run"), Atom("tgt")});
    bool ok = !o.notes->got.empty();
    for (auto& m : o.notes->got) {
        std::string i = m[1].str();
        int midi = musil::pitch_to_midi(m[2].str());
        ok = ok && (i == "Vc" || (i == "Vn" && midi >= 62 && midi <= 64)) && m[9].f >= 0 && m[9].f <= 3;
    }
    CHECK(ok);
    o.clear();
    o.orchestra({Atom("techniques"), Atom("pizz")});          // no such technique in MicroSOL: a rest, said, no notes
    o.send({Atom("run"), Atom("tgt")});
    CHECK(o.notes->got.empty());
    CHECK(o.r.printedSomething("orchidea: segment 1 is a rest: empty search space"));
    CHECK(o.done->got.size() == 1);                             // the run went on (a rest is a result)
    o.clear();
    o.send({Atom("report")});
    CHECK(o.r.printedSomething("1: at 0.00 s, 6.19 s; a rest: empty search space"));
    jobsSynchronous = sync;
}

// a filter that empties one segment makes it a rest (no notes, a message, a line in the report), the other segments
// are orchestrated: the run is not lost (it failed with "empty search space" before)
TEST(orchidea_an_empty_segment_is_a_rest) {
    // six sounds, three segments; the second segment's partials are a pitch nobody plays (its pitch filter empties it)
    musil::orchidea::search_options so;
    so.p.pop_size = 20; so.p.max_epochs = 5; so.p.seed = 3;
    std::vector<musil::orchidea::entry> db;
    for (int k = 0; k < 6; ++k) {
        musil::orchidea::entry e;
        e.index = k; e.instr = k < 3 ? "Ob" : "Vc"; e.pitch = k % 3 == 0 ? "C4" : k % 3 == 1 ? "D4" : "E4"; e.midi = 60 + 2 * (k % 3);
        e.features.assign(8, 0.f);
        e.features[(size_t)k] = 1.f;
        db.push_back(e);
    }
    std::vector<musil::orchidea::segment> segs(3);
    for (size_t k = 0; k < 3; ++k) { segs[k].start = (double)k; segs[k].length = 1; segs[k].features.assign(8, 0.f); segs[k].features[k] = 1.f; }
    segs[1].notes["G9"] = 0;                                   // no sound at G9: segment 2 is a rest
    auto r = musil::orchidea::search_segments(db, {"Ob", "Vc"}, segs, so);
    CHECK(r.segments.size() == 3 && r.choices.size() == 3);
    CHECK(r.segments[0].rest.empty() && r.segments[2].rest.empty());
    CHECK(r.segments[1].rest.find("empty search space") != std::string::npos);
    CHECK(r.segments[1].solutions.size() == 1 && r.segments[1].used == 0);
    CHECK(r.segments[0].used > 0 && r.segments[2].used > 0);
    for (auto c : {"closest", "best", "path"}) {               // every connection goes through a rest
        so.connection = c;
        auto q = musil::orchidea::search_segments(db, {"Ob", "Vc"}, segs, so);
        CHECK(q.choices.size() == 3 && q.segments[1].used == 0);
    }
}

// a piano phrase on MicroSOL with the pitch filter on (partials 0.3, the segmentation at the flux's onsets): some
// segments have no partial MicroSOL plays (a rest each, said), the others are orchestrated at the partials' cents
// (the run failed with "empty search space" before, and the notes kept their cents of 0 from an earlier run)
TEST(orchidea_microsol_with_the_pitch_filter) {
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    OrchRig o("", "data/A_minor.wav");
    o.send({Atom("segmentation"), Atom("flux")});
    o.send({Atom("threshold"), Atom(.1f)});
    o.send({Atom("partials"), Atom(.3f)});
    o.send({Atom("population"), Atom(16.f)});
    o.send({Atom("epochs"), Atom(3.f)});
    o.send({Atom("run"), Atom("tgt")});
    jobsSynchronous = sync;
    CHECK(o.done->got.size() == 1 && !o.notes->got.empty());
    CHECK(o.r.printedSomething("is a rest: empty search space"));
    bool cents = false;
    for (auto& m : o.notes->got) cents = cents || (m.size() == 10 && m[8].f != 0.f);
    CHECK(cents);
}

// with the path connection and sustain (the dynamic defaults of a segmented target), notes are held across the
// segments and the report says how many (it said 0 holding on, always, before)
TEST(orchidea_path_and_sustain_hold_notes) {
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    OrchRig o;
    o.send({Atom("partials"), Atom(0.f)});
    o.send({Atom("population"), Atom(30.f)});
    o.send({Atom("epochs"), Atom(10.f)});
    o.send({Atom("seed"), Atom(3.f)});
    o.send({Atom("onsets"), Atom(0.f), Atom(500.f), Atom(1000.f), Atom(1500.f)});
    o.send({Atom("connection"), Atom("path")});
    o.send({Atom("sustain"), Atom(1.f)});
    o.send({Atom("run"), Atom("tgt")});
    CHECK(o.done->got.size() == 1 && !o.notes->got.empty());
    bool longer = false;                                        // a note held across a boundary is one longer note
    for (auto& m : o.notes->got) longer = longer || (m.size() == 10 && m[5].f < 1400.f && m[6].f > 501.f);
    CHECK(longer);
    o.clear();
    o.send({Atom("report")});
    int held = 0;
    for (auto& line : o.r.printed) {
        size_t p = line.find(" holding on");
        if (p == std::string::npos) continue;
        size_t q = line.rfind(' ', p - 1);
        held += std::atoi(line.substr(q + 1, p - q - 1).c_str());
    }
    CHECK(held > 0);
    jobsSynchronous = sync;
}

// bad inputs: a message each, never a crash
TEST(orchidea_bad_inputs) {
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    {
        OrchRig o;
        o.send({Atom("run"), Atom("nothere")});
        CHECK(o.r.printedSomething("no sound in buffer~ nothere"));
        o.send({Atom("run"), Atom("tgt"), Atom("nobuffer")});
        CHECK(o.r.printedSomething("no buffer~ nobuffer"));
        o.send({Atom("choose"), Atom(1.f), Atom(1.f)});
        CHECK(o.r.printedSomething("nothing orchestrated yet"));
        o.send({Atom("write"), Atom("x.txt")});
        o.send({Atom("connection"), Atom("sideways")});
        CHECK(o.r.printedSomething("connection is closest, best or path"));
        // absurd parameters are brought into range
        o.clear();
        o.send({Atom("partials"), Atom(0.f)});
        o.send({Atom("population"), Atom(-5.f)});
        o.send({Atom("epochs"), Atom(1e12f)});
        o.send({Atom("epochs"), Atom(3.f)});
        o.send({Atom("sparsity"), Atom(7.f)});
        o.send({Atom("mutation"), Atom(-1.f)});
        o.send({Atom("window"), Atom(1e12f)});
        o.send({Atom("window"), Atom(4096.f)});
        o.send({Atom("seed"), Atom(std::nanf(""))});
        o.send({Atom("run"), Atom("tgt"), Atom("mix")});
        CHECK(o.done->got.size() == 1);
        o.send({Atom("choose"), Atom(5.f), Atom(1.f)});
        CHECK(o.r.printedSomething("no segment 5"));
        o.send({Atom("choose"), Atom(1.f), Atom(500.f)});
        CHECK(o.r.printedSomething("solutions"));
        o.send({Atom("write"), Atom("/nonexistent-folder-moritz/x.txt")});
        CHECK(o.r.printedSomething("not written"));        // outside the patch's folder and ~/Documents/Moritz
    }
    {   // an empty target buffer~, then a target of a few samples and a silent one
        OrchRig o("", "");
        o.send({Atom("run"), Atom("tgt")});
        CHECK(o.r.printedSomething("no sound in buffer~ tgt"));
        for (float v : {0.f, .5f}) {
            SampleBuffer s;
            s.frames = v == 0.f ? 44100 : 10;
            s.sr = 44100;
            s.data.assign((size_t)s.frames, v);
            findBuffer(&o.r.p, "tgt")->replace(std::move(s));
            o.clear();
            o.send({Atom("run"), Atom("tgt"), Atom("mix")});
            CHECK(o.done->got.size() == 1);
        }
    }
    {   // no orchestra; an orchestra without a database; a database that is not there; one that is not a database
        Rig r("(patch (obj t buffer~ tgt data/archeos-bell.wav) (obj a orchidea nobody) (obj or orchestra empty)"
              " (obj b orchidea empty))");
        r.box("a")->message(0, {Atom("run"), Atom("tgt")});
        CHECK(r.printedSomething("orchidea: no orchestra nobody"));
        r.box("b")->message(0, {Atom("run"), Atom("tgt")});
        CHECK(r.printedSomething("has no database"));
        r.box("or")->message(0, {Atom("read"), Atom("/nonexistent-folder-moritz/x.db")});
        CHECK(r.printedSomething("cannot open"));
        std::string bad = tempPath("moritz-orchidea-bad.db");
        for (const char* text : {"garbage\n", "spectrum 3 1 2\n/a/Vn-ord-C4-mf.wav;1;2\n", "spectrum 2048 256 4\n/a/Vn-ord-C4-mf.wav;1;2\n",
                                 "spectrum 2048 256 99999999999\n"}) {
            { std::ofstream f(bad); f << text; }
            r.printed.clear();
            r.box("or")->message(0, {Atom("read"), Atom(bad)});
            r.box("b")->message(0, {Atom("run"), Atom("tgt")});
            CHECK(!r.printed.empty());
        }
        std::remove(bad.c_str());
        r.box("or")->message(0, {Atom("players"), Atom("Xyz")});
        r.box("or")->message(0, {Atom("range"), Atom("Vn"), Atom("nopitch"), Atom("C4")});
        CHECK(r.printedSomething("pitches are names"));
        r.box("or")->message(0, {Atom("info")});
    }
    jobsSynchronous = sync;
}

// the solution written as an Orchidea connection (temporary file, then renamed): what score-load reads
TEST(orchidea_writes_the_connection) {
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    OrchRig o;
    o.send({Atom("partials"), Atom(0.f)});
    o.send({Atom("run"), Atom("tgt")});
    std::string path = tempPath("moritz-orchidea-connection.txt");
    o.send({Atom("write"), Atom(path)});                    // the patch has no folder: written nowhere
    CHECK(o.r.printedSomething("not written") && !std::ifstream(path));
    o.r.p.dir = path.substr(0, path.find_last_of('/'));     // the patch's folder: where it may write
    o.send({Atom("write"), Atom(path)});
    jobsSynchronous = sync;
    std::ifstream f(path);
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(all.rfind("[ orchestra ", 0) == 0);
    size_t count = 0;
    for (size_t p = all.find("[ note "); p != std::string::npos; p = all.find("[ note ", p + 1)) ++count;
    CHECK(count == o.notes->got.size() && count > 0);
    std::ifstream tmp(path + ".tmp");
    CHECK(!tmp);
    std::remove(path.c_str());
}

// a run in the background: stop cancels it (no notes, no done), the next run works; a box deleted while its job runs
// cancels it and waits; a run sent while the orchestra still reads its database waits for it
TEST(orchidea_runs_in_the_background) {
    bool sync = jobsSynchronous;
    jobsSynchronous = false;
    auto drainUntil = [](const std::function<bool()>& cond) {
        for (int k = 0; k < 6000 && !cond(); ++k) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            drainMain();
        }
    };
    {
        OrchRig o;
        // a run sent at once waits for the database being read
        o.send({Atom("partials"), Atom(0.f)});
        o.send({Atom("population"), Atom(40.f)});
        o.send({Atom("epochs"), Atom(20.f)});
        o.send({Atom("run"), Atom("tgt"), Atom("mix")});
        drainUntil([&] { return !o.done->got.empty(); });
        CHECK(o.done->got.size() == 1);
        CHECK(!o.notes->got.empty());
        // a long run, stopped
        o.clear();
        o.send({Atom("population"), Atom(20000.f)});
        o.send({Atom("epochs"), Atom(100000.f)});
        o.send({Atom("patience"), Atom(100000.f)});
        o.send({Atom("run"), Atom("tgt"), Atom("mix")});
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        drainMain();
        o.send({Atom("stop")});
        for (int k = 0; k < 20; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); drainMain(); }
        CHECK(o.done->got.empty());
        CHECK(o.notes->got.empty());
        // the next run works
        o.send({Atom("population"), Atom(40.f)});
        o.send({Atom("epochs"), Atom(20.f)});
        o.send({Atom("patience"), Atom(150.f)});
        o.send({Atom("run"), Atom("tgt"), Atom("mix")});
        drainUntil([&] { return !o.done->got.empty(); });
        CHECK(o.done->got.size() == 1);
        // deleted while running
        o.send({Atom("population"), Atom(20000.f)});
        o.send({Atom("epochs"), Atom(100000.f)});
        o.send({Atom("patience"), Atom(100000.f)});
        o.send({Atom("run"), Atom("tgt"), Atom("mix")});
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto t0 = std::chrono::steady_clock::now();
        o.r.p.removeBoxes({o.r.box("oi")});
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(secs < 5);
        for (int k = 0; k < 20; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); drainMain(); }
    }
    {   // the orchestra deleted while a run waits for its database: the run is dropped
        OrchRig o;
        o.send({Atom("run"), Atom("tgt")});
        o.r.p.removeBoxes({o.r.box("or")});
        for (int k = 0; k < 50; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); drainMain(); }
        CHECK(o.done->got.empty());
    }
    {   // the orchidea deleted while its run waits for the database: nothing runs after
        OrchRig o;
        o.send({Atom("run"), Atom("tgt")});
        o.r.p.removeBoxes({o.r.box("oi")});
        for (int k = 0; k < 200; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); drainMain(); }
        CHECK(o.done->got.empty());
    }
    jobsSynchronous = sync;
}

// the solution as a score, to a score box: a note a message of the notes outlet (instrument, pitch, dynamics,
// technique, onset, duration, the sample, cents), a row an instrument in orchestral order, and the box plays it
TEST(orchidea_solution_as_a_score) {
    namespace sc = musil::score;
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    OrchRig o(" (obj s score solution 1) (obj tr transport 60) (obj dac dac~ 1) (connect oi 4 s 0) (connect s 0 dac 0)");
    o.send({Atom("partials"), Atom(0.f)});
    o.send({Atom("seed"), Atom(3.f)});
    o.send({Atom("run"), Atom("tgt")});
    jobsSynchronous = sync;
    Box* src = o.r.box("s");
    CHECK(src != nullptr && isScoreBox(src));
    if (!src) return;
    auto s = scoreOfBox(src);
    CHECK(!o.notes->got.empty() && s->size() == o.notes->got.size());
    bool same = true;
    size_t k = 0;
    for (auto* e : s->byTime()) {
        if (k >= o.notes->got.size()) break;
        const Message& m = o.notes->got[k++];
        const sc::Datum* snd = sc::field(e->extra, "sound");
        same = same && e->kind == sc::Kind::Note && e->row == m[1].str() && e->pitch == sc::pitchOfName(m[2].str()) &&
               e->dyn == m[3].str() && e->tech == m[4].str() && std::fabs(e->at * 1000 - m[5].f) < 0.01 &&
               std::fabs(e->dur * 1000 - m[6].f) < 0.01 && snd && snd->s == m[7].str() && e->cents == m[8].f;
    }
    CHECK(same);
    int last = -1;
    bool ordered = true;
    for (auto& r : s->rows) {
        int rank = sc::layout::instrumentRank(r.name);
        ordered = ordered && rank > last && (r.clef == "treble" || r.clef == "bass");
        last = rank;
    }
    CHECK(!s->rows.empty() && ordered);
    o.send({Atom("score")});                         // again: the same score
    CHECK(sc::toText(*scoreOfBox(src)) == sc::toText(*s));
    o.r.box("tr")->message(0, {Atom("play")});
    o.r.ch = 1;
    auto y = o.r.run(44100);
    double energy = 0;
    for (float v : y) energy += std::fabs(v);
    CHECK(energy > 1);
    CHECK(!o.r.printedSomething("score:"));
}

// a target cut into segments is searched with Musil's dynamic settings, unless a message chose them; a static target
// keeps Orchidea's
TEST(orchidea_dynamic_defaults) {
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    {
        OrchRig o;
        o.send({Atom("partials"), Atom(0.f)});
        o.send({Atom("population"), Atom(30.f)});
        o.send({Atom("epochs"), Atom(20.f)});
        o.send({Atom("run"), Atom("tgt")});
        o.send({Atom("report")});
        CHECK(o.r.printedSomething("segmentation none, population 30, epochs 20, sparsity 0.001"));
        CHECK(o.r.printedSomething("connection closest"));
    }
    {
        OrchRig o;
        o.send({Atom("segmentation"), Atom("flux")});
        o.send({Atom("threshold"), Atom(0.5f)});
        o.send({Atom("population"), Atom(30.f)});
        o.send({Atom("epochs"), Atom(20.f)});
        o.send({Atom("connection"), Atom("best")});
        o.send({Atom("run"), Atom("tgt")});
        o.send({Atom("report")});
        // population, epochs and connection as the messages set them; the rest Musil's dynamic settings (MicroSOL:
        // no pitch filter)
        CHECK(o.r.printedSomething("segmentation flux, population 30, epochs 20, sparsity 0.01, partials 0, solutions 5, sustain 1, "
                                   "hold 4000 ms, dovetail 0.75, hysteresis 0, connection best"));
    }
    jobsSynchronous = sync;
}

namespace {
// a score's notes, every field the connection sets, in order
std::string noteKeys(const musil::score::Score& s) {
    namespace sc = musil::score;
    std::vector<std::string> v;
    for (auto& e : s.events()) {
        const sc::Datum* snd = sc::field(e->extra, "sound");
        const sc::Datum* pl = sc::field(e->extra, "player");
        v.push_back(sc::numText(e->at) + " " + e->row + " " + sc::numText(e->pitch) + " " + sc::numText(e->dur) + " " + sc::numText(e->cents) +
                    " " + e->dyn + " " + e->tech + " " + sc::numText(e->az) + " " + (snd ? snd->s : "-") + " " + (pl ? sc::numText(pl->num) : "-"));
    }
    std::sort(v.begin(), v.end());
    std::string t;
    for (auto& x : v) t += x + "\n";
    return t;
}
// the score as text but for the next id (an undo does not give ids back)
std::string withoutNextId(const musil::score::Score& s) {
    std::string t = musil::score::toText(s);
    size_t k = t.find("(next-id ");
    if (k != std::string::npos) t.erase(k, t.find(')', k) - k + 1);
    return t;
}
}  // namespace

// A target cut into segments: the score keeps every segment's solutions (the one chosen in each); another solution
// chosen in the roll (one undo step, undone to the score as it was) and orchidea's choose give the same notes, which
// are those of the connection orchidea makes again with that choice (score); the notes outlet says as many
TEST(orchidea_choose_and_the_roll_agree) {
    namespace sc = musil::score;
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    OrchRig o(" (obj s score solution 1) (connect oi 4 s 0)");
    o.send({Atom("partials"), Atom(0.f)});
    o.send({Atom("population"), Atom(30.f)});
    o.send({Atom("epochs"), Atom(10.f)});
    o.send({Atom("seed"), Atom(3.f)});
    o.send({Atom("onsets"), Atom(0.f), Atom(500.f), Atom(1000.f), Atom(1500.f)});
    o.send({Atom("run"), Atom("tgt")});
    Box* box = o.r.box("s");
    auto first = scoreOfBox(box)->clone();
    auto os = sc::solutions::read(*first);
    CHECK(os.size() == 1 && os[0].segments.size() == 4);
    if (os.size() != 1) { jobsSynchronous = sync; return; }
    int compared = 0, moved = 0;
    for (size_t k = 0; k < os[0].segments.size(); ++k) {
        const auto& g = os[0].segments[k];
        for (size_t rank = 0; rank < g.solutions.size() && rank < 3; ++rank) {
            if ((int)rank == g.chosen) continue;
            auto doc = ScoreDoc::ofBox(box);
            auto t = makeRollTarget(doc);
            std::string before = withoutNextId(*t->score());
            if (!t->chooseSolution({}, 0, k, rank)) continue;        // the same notes (nothing to do)
            std::string roll = noteKeys(*t->score());
            CHECK(doc->undoName() == "choose solution" && t->undo() && withoutNextId(*t->score()) == before);
            o.clear();
            o.send({Atom("choose"), Atom((float)k + 1), Atom((float)rank + 1)});
            auto chosen = scoreOfBox(box);
            CHECK(noteKeys(*chosen) == roll);
            CHECK(sc::solutions::read(*chosen)[0].segments[k].chosen == (int)rank);
            CHECK(o.notes->got.size() == chosen->size());
            o.send({Atom("score")});                                 // the connection made again: the same notes
            CHECK(noteKeys(*scoreOfBox(box)) == roll);
            moved += roll != noteKeys(*first);
            ++compared;
            o.send({Atom("choose"), Atom((float)k + 1), Atom((float)g.chosen + 1)});   // back
            CHECK(noteKeys(*scoreOfBox(box)) == noteKeys(*first));
        }
    }
    CHECK(compared >= 3 && moved >= 3);
    jobsSynchronous = sync;
}
