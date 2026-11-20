// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// score_tests.cpp — transport and score: a score's sounds and notes on their sample (offline and with the device's
// buffers), locate, loop and stop, a score replaced and edited while the audio thread plays it, and Musil's own
// C++ score (.score files read back the same, untrusted text refused without a crash).

#include "rig.h"
#include "transport.h"

#include "music/score.h"
#include "music/score_text.h"

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace moritz;
using etest::Rig;
namespace sc = musil::score;

namespace {

// a buffer~ of one impulse (1 at frame 0, then nothing): a sound of it is a click at its onset
void impulseBuffer(Rig& r, const char* id, long frames = 4410) {
    auto* b = dynamic_cast<BufferBox*>(r.box(id));
    SampleBuffer s;
    s.channels = 1;
    s.sr = r.sr;
    s.frames = frames;
    s.data.assign((size_t)frames, 0.f);
    s.data[0] = 1.f;
    b->replace(std::move(s));
}
std::shared_ptr<sc::Event> sound(double at, double dur, const char* buffer, double gain = 1) {
    auto e = std::make_shared<sc::Event>();
    e->kind = sc::Kind::Sound;
    e->buffer = true;
    e->source = buffer;
    e->at = at;
    e->dur = dur;
    e->gain = gain;
    return e;
}
std::shared_ptr<sc::Event> note(double at, double dur, double pitch) {
    auto e = std::make_shared<sc::Event>();
    e->kind = sc::Kind::Note;
    e->at = at;
    e->dur = dur;
    e->pitch = pitch;
    e->row = "Cl";
    e->dyn = "mf";
    return e;
}
// sends a C++ score to a score box, as a code box does
void give(Rig& r, const char* id, std::shared_ptr<sc::Score> s) {
    r.box(id)->message(0, {Atom("score"), Atom(shareScore(s))});
}
std::vector<long> peaks(const std::vector<float>& y, float above = 0.5f) {
    std::vector<long> at;
    for (size_t i = 0; i < y.size(); ++i)
        if (y[i] > above) at.push_back((long)i);
    return at;
}
// a score box (mono) into the left channel, its notes into a click~ on the right
const char* kPatch =
    "(patch (obj b buffer~ imp 100) (obj s score x 1) (obj tb t b) (obj c click~) (obj o dac~ 1 2)"
    " (obj tr transport 240 4 4)"
    " (connect s 0 o 0) (connect s 1 tb 0) (connect tb 0 c 0) (connect c 0 o 1))";

}  // namespace

// Offline: a sound and a note of the score at their samples (floor(onset * rate)), counted from the sample where
// play landed; a note's click on the very sample of its onset
TEST(score_sounds_and_notes_land_on_their_sample) {
    Rig r(kPatch);
    impulseBuffer(r, "b");
    auto s = std::make_shared<sc::Score>("x", 44100);
    s->insert(sound(0.5, 0.1, "imp"));
    s->insert(sound(0.123456, 0.1, "imp", 0.75));
    s->insert(note(0.25, 0.1, 60));
    s->insert(note(0.7071, 0.1, 62));
    give(r, "s", s);
    r.run(1000);                                     // play lands at sample 1000
    r.box("tr")->message(0, {Atom("play")});
    auto y = r.run(44100);
    auto snd = peaks(etest::channel(y, 2, 0), 0.5f);
    auto clk = peaks(etest::channel(y, 2, 1), 0.5f);
    CHECK(snd.size() == 2);
    CHECK(snd.size() == 2 && snd[0] == 5444 && snd[1] == 22050);   // floor(0.123456 * 44100) = 5444, first (0.75 > 0.5)
    CHECK(clk.size() == 2);
    CHECK(clk.size() == 2 && clk[0] == 11025 && clk[1] == 31183);  // floor(0.7071 * 44100) = 31183
    CHECK(!r.printedSomething("score:"));
}

// The same with the device's buffers of 512 frames and the GUI's timers firing late between them: the sound on its
// sample, the note's click on its sample too (two buffers after the play message, as every timed event in real time)
TEST(score_lands_on_its_sample_in_real_time) {
    const int P = 512, sr = 44100;
    double wall = 0;
    Rig r("");
    r.p.offline = false;
    r.p.clock = [&wall] { return wall; };
    r.p.ioBufferFrames = P;
    std::string err;
    r.p.load(kPatch, err);
    r.p.loadbangAll();
    impulseBuffer(r, "b");
    auto s = std::make_shared<sc::Score>("x", sr);
    s->insert(sound(0.5, 0.1, "imp"));
    s->insert(note(0.5, 0.1, 60));
    give(r, "s", s);
    std::vector<float> out, buf((size_t)P * 2);
    long S = 0;
    r.p.process(nullptr, 0, buf.data(), 2, P);       // the first callback anchors the clock
    S += P;
    out.insert(out.end(), buf.begin(), buf.end());
    wall = (double)S / sr;
    r.box("tr")->message(0, {Atom("play")});         // lands two buffers later: at S + 2 P
    long playAt = S + 2 * P;
    uint32_t rng = 9;
    for (int k = 0; k < 120; ++k) {
        double due = r.p.nextDue();
        while (due * sr < S + P) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            double fireAt = due + (rng % 401) / (double)sr;
            if (fireAt * sr >= S + P) break;
            wall = fireAt;
            r.p.runDue(wall);
            due = r.p.nextDue();
        }
        wall = (double)S / sr;
        r.p.process(nullptr, 0, buf.data(), 2, P);
        out.insert(out.end(), buf.begin(), buf.end());
        S += P;
        wall = (double)(S - 1) / sr;
        r.p.runDue(wall);
    }
    auto snd = peaks(etest::channel(out, 2, 0));
    auto clk = peaks(etest::channel(out, 2, 1));
    CHECK(snd.size() == 1 && snd[0] == playAt + 22050);
    CHECK(clk.size() == 1 && clk[0] == playAt + 22050);
}

// locate (bars at the transport's tempo: 240 bpm, a bar is a second), loop and stop
TEST(transport_locate_loop_and_stop) {
    Rig r(kPatch);
    impulseBuffer(r, "b");
    auto s = std::make_shared<sc::Score>("x", 44100);
    s->insert(sound(0.25, 0.1, "imp"));
    s->insert(sound(1.25, 0.1, "imp"));
    s->insert(note(0.5, 0.1, 60));
    s->insert(note(1.5, 0.1, 60));
    give(r, "s", s);
    // from bar 2: only what is after one second, 0.25 s after the play
    r.box("tr")->message(0, {Atom("locate"), Atom(2.f)});
    r.box("tr")->message(0, {Atom("play")});
    auto y = r.run(44100);
    auto snd = peaks(etest::channel(y, 2, 0));
    auto clk = peaks(etest::channel(y, 2, 1));
    CHECK(snd.size() == 1 && snd[0] == 11025);
    CHECK(clk.size() == 1 && clk[0] == 22050);
    // a loop over bar 1: from now (beyond its end, so it does not wrap) then a locate into it wraps every second
    r.box("tr")->message(0, {Atom("loop"), Atom(1.f), Atom(2.f)});
    r.box("tr")->message(0, {Atom("locate"), Atom(1.f)});
    long base = r.frame;
    y = r.run(3 * 44100);
    snd = peaks(etest::channel(y, 2, 0));
    clk = peaks(etest::channel(y, 2, 1));
    CHECK(snd.size() == 3 && snd[0] == 11025 && snd[1] == 44100 + 11025 && snd[2] == 2 * 44100 + 11025);
    CHECK(clk.size() == 3 && clk[0] == 22050 && clk[1] == 44100 + 22050 && clk[2] == 2 * 44100 + 22050);
    (void)base;
    // stopped: nothing more
    r.box("tr")->message(0, {Atom("stop")});
    y = r.run(2 * 44100);
    CHECK(peaks(etest::channel(y, 2, 0)).empty());
    CHECK(peaks(etest::channel(y, 2, 1)).empty());
}

// The transport's outlets: bar and beat at every beat (240 bpm: four a second), the position in ms, the state
TEST(transport_counts_bars_and_beats) {
    Rig r("(patch (obj tr transport 240 4 4) (obj pb print bar) (obj pt print beat) (obj ps print state)"
          " (connect tr 0 pb 0) (connect tr 1 pt 0) (connect tr 3 ps 0))");
    r.box("tr")->message(0, {Atom("play")});
    r.run(44100 * 3 / 2 + 10);
    int bars = 0, beats = 0;
    for (auto& p : r.printed) {
        if (p.rfind("bar:", 0) == 0) ++bars;
        if (p.rfind("beat:", 0) == 0) ++beats;
    }
    CHECK(beats == 7);                               // at play, then at 0.25 0.5 ... 1.5
    CHECK(r.printedSomething("bar: 2"));
    CHECK(r.printedSomething("state: 1"));
    CHECK(bars == beats);
}

// The audio thread plays the score while the GUI thread replaces it, adds to it, clears it, moves the transport:
// nothing allocated or freed there, nothing torn (ThreadSanitizer); what the GUI let go is freed once the audio
// thread has let go of it
TEST(score_edited_while_audio_runs) {
    Rig r(kPatch);
    impulseBuffer(r, "b");
    r.box("tr")->message(0, {Atom("loop"), Atom(1.f), Atom(2.f)});
    r.box("tr")->message(0, {Atom("play")});
    std::atomic<bool> stop{false};
    std::atomic<long> blocks{0};
    std::thread audio([&] {
        std::vector<float> buf(2 * 64);
        while (!stop) {
            r.p.process(nullptr, 0, buf.data(), 2, 64);
            blocks.fetch_add(1);
        }
    });
    for (int k = 0; k < 400; ++k) {
        auto s = std::make_shared<sc::Score>("x", 44100);
        for (int j = 0; j < 40; ++j) s->insert(sound(j * 0.02, 0.05, "imp", 0.5));
        for (int j = 0; j < 10; ++j) s->insert(note(j * 0.1, 0.05, 60 + j));
        give(r, "s", s);
        r.box("s")->message(0, {Atom("add"), Atom("note"), Atom(0.33f), Atom(0.1f), Atom("Fl"), Atom("G5")});
        if (k % 7 == 0) r.box("s")->message(0, {Atom("clear")});
        if (k % 11 == 0) r.box("tr")->message(0, {Atom("locate"), Atom(1.f), Atom((float)(1 + k % 4))});
        if (k % 50 == 0) impulseBuffer(r, "b");
        r.p.runDue(r.p.time() + 0.01);
        r.p.collectGarbage();
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    stop = true;
    audio.join();
    r.p.collectGarbage(true);
    CHECK(blocks.load() > 0);
    CHECK(!r.printedSomething("score:"));
}

// A box deleted while the audio thread plays it, and the patch closed while it plays
TEST(score_box_removed_while_audio_runs) {
    auto r = std::make_unique<Rig>(kPatch);
    impulseBuffer(*r, "b");
    auto s = std::make_shared<sc::Score>("x", 44100);
    for (int j = 0; j < 100; ++j) s->insert(sound(j * 0.01, 0.05, "imp"));
    give(*r, "s", s);
    r->box("tr")->message(0, {Atom("play")});
    std::atomic<bool> stop{false};
    std::thread audio([&] {
        std::vector<float> buf(2 * 64);
        while (!stop) r->p.process(nullptr, 0, buf.data(), 2, 64);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    r->p.removeBoxes({r->box("s")});
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    r->p.collectGarbage();
    stop = true;
    audio.join();
    r->p.collectGarbage(true);
    r.reset();
    CHECK(true);
}

// Playing a score allocates nothing on the audio thread
TEST(score_plays_without_allocating) {
    if (!etest::allocGuardActive()) {
        std::printf("  (skipped: the sanitizer's runtime owns operator new)\n");
        return;
    }
    Rig r(kPatch);
    impulseBuffer(r, "b");
    auto s = std::make_shared<sc::Score>("x", 44100);
    for (int j = 0; j < 200; ++j) s->insert(sound(j * 0.005, 0.05, "imp"));
    for (int j = 0; j < 20; ++j) s->insert(note(j * 0.05, 0.05, 60));
    give(r, "s", s);
    r.box("tr")->message(0, {Atom("play")});
    long before = etest::audioAllocations();
    r.run(44100, 37);
    CHECK(etest::audioAllocations() == before);
}

// A score kept in the patch: saved inside it, read back the same
TEST(score_kept_in_the_patch) {
    Rig r("(patch (obj s score piece 2))");
    r.box("s")->message(0, {Atom("add"), Atom("note"), Atom(0.5f), Atom(0.25f), Atom("Fl"), Atom("G5"), Atom("mf"), Atom("ord")});
    r.box("s")->message(0, {Atom("add"), Atom("msg"), Atom(1.f), Atom("tonic"), Atom(62.f)});
    std::string text = r.p.serialize();
    CHECK(text.find("(script") != std::string::npos);
    Rig back(text);
    CHECK(back.p.serialize() == text);
    CHECK(back.error.empty());
}

// A .score file next to the patch: read, played, written back the same (a file elsewhere is not written)
TEST(score_file_read_and_written) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("moritz_score_" + std::to_string(std::rand()));
    fs::create_directories(dir);
    std::string text =
        "(score \"piece\" (sr 44100) (bpm 108) (meter 4 4) (next-id 4)\n"
        "  (row Cl treble)\n"
        "  (note 0.5 0.25 Cl 79 mf ord (id 1))\n"
        "  (sound 1 0.5 (buffer \"imp\") (id 2) (az 30))\n"
        "  (msg 1.5 density 0.6 (id 3)))\n";
    {
        std::ofstream f(dir / "piece.score");
        f << text;
    }
    Rig r("");
    r.p.dir = dir.string();
    std::string err;
    r.p.load("(patch (obj b buffer~ imp 100) (obj s score piece piece.score) (obj pm print m) (obj o dac~ 1 2)"
           " (connect s 3 pm 0) (connect s 0 o 0) (connect s 1 o 1))", err);
    r.p.loadbangAll();
    impulseBuffer(r, "b");
    r.box("s")->message(0, {Atom("play")});
    auto y = r.run(2 * 44100);
    CHECK(r.printedSomething("m: density 0.6"));
    // the sound at 30 degrees to the left: more on the left
    auto L = peaks(etest::channel(y, 2, 0), 0.1f), R = peaks(etest::channel(y, 2, 1), 0.1f);
    CHECK(L.size() == 1 && L[0] == 44100);
    CHECK(R.size() == 1 && R[0] == 44100);
    CHECK(etest::channel(y, 2, 0)[44100] > etest::channel(y, 2, 1)[44100]);
    r.box("s")->message(0, {Atom("write"), Atom("copy.score")});
    std::string a, b;
    readFile((dir / "piece.score").string(), a);
    readFile((dir / "copy.score").string(), b);
    auto sa = sc::readText(a, "a"), sb = sc::readText(b, "b");
    CHECK(sc::toText(*sa) == sc::toText(*sb));
    r.box("s")->message(0, {Atom("write"), Atom("/elsewhere-not-allowed.score")});
    CHECK(r.printedSomething("not written"));
    fs::remove_all(dir);
}

// Musil's score in C++: written, read back the same; placements compose; a score cannot contain itself; text from
// elsewhere that is wrong is an error, never a crash
TEST(musil_score_structure) {
    auto inner = std::make_shared<sc::Score>("inner", 44100);
    inner->insert(note(0, 1, 60));
    inner->insert(note(1, 1, 62));
    auto outer = std::make_shared<sc::Score>("outer", 44100);
    auto pl = std::make_shared<sc::Event>();
    pl->kind = sc::Kind::Score;
    pl->sub = inner;
    pl->at = 2;
    pl->dur = 3;
    pl->stretch = 2;
    pl->transpose = 7;
    pl->gain = 0.5;
    outer->insert(pl);
    std::vector<sc::Placed> flat;
    sc::flatten(*outer, flat);
    CHECK(flat.size() == 2);
    CHECK(flat.size() == 2 && flat[1].ev.at == 4 && flat[1].ev.dur == 1 && flat[1].ev.pitch == 69 && flat[1].ev.gain == 0.5);
    bool refused = false;
    auto back = std::make_shared<sc::Event>();
    back->kind = sc::Kind::Score;
    back->sub = outer;
    try { inner->insert(back); } catch (const sc::Error&) { refused = true; }
    CHECK(refused);
    std::string t = sc::toText(*outer);
    CHECK(sc::toText(*sc::readText(t, "t")) == t);
    int errors = 0;
    for (std::string bad : {std::string("(score \"x\" (note 0 1e999 a 60))"), std::string(4000, '('), std::string("(score"),
                            std::string("(score \"x\" (note 0 1 a))"), std::string("(score 1 2 3)"), std::string("\"\\")}) {
        try { sc::readText(bad, "bad"); } catch (const sc::Error&) { ++errors; }
    }
    CHECK(errors == 6);
    std::vector<sc::Event> got;
    outer->query(2.5, 2.6, [&](const sc::Event& e) { got.push_back(e); });
    CHECK(got.size() == 1);
}
