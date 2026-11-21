// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// orchidea_tests.cpp — orchestra and orchidea (objects_orchidea.cpp): a target orchestrated on MicroSOL, the notes and
// the mix; the same seed the same notes; the players, ranges and techniques obeyed; the job cancelled, its box deleted
// while it runs, a run waiting for its orchestra's database; bad inputs answered with a message, never a crash

#include "musil_host.h"
#include "roll_edit.h"
#include "transport.h"

#include "music/score_layout.h"
#include "rig.h"

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
    o.orchestra({Atom("techniques"), Atom("pizz")});          // no such technique in MicroSOL: a message, no notes
    o.send({Atom("run"), Atom("tgt")});
    CHECK(o.notes->got.empty());
    CHECK(o.r.printedSomething("orchidea: segment 1: empty search space"));
    CHECK(o.done->got.empty());                                 // a run that failed does not bang done
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
        CHECK(o.r.printedSomething("can't write"));
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
