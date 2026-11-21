// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// score_tests.cpp — transport and score: a score's sounds and notes on their sample (offline and with the device's
// buffers), locate, loop and stop, a score replaced and edited while the audio thread plays it, and Musil's own
// C++ score (.score files read back the same, untrusted text refused without a crash).

#include "rig.h"
#include "gui.h"
#include "preview.h"
#include "score_doc.h"
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

// A loop never stalls, whatever the time play starts at (a time not exact in binary: 0.1 s after a loadbang, 0.25 s
// and the passes after it, and many others): its beats keep coming, one a beat, and the score's notes on every pass
TEST(transport_loop_never_stalls) {
    const long starts[] = {4410, 11025, 4410 * 3, 1, 977, 12345, 22051, 30001, 44099, 7777, 100003, 2 * 4410 + 13};
    int bad = 0;
    for (long start : starts) {
        Rig r("(patch (obj tr transport 240 4 4) (obj pt print beat) (connect tr 1 pt 0) (obj s score x 1)"
              " (obj pn print note) (connect s 1 pn 0))");
        auto sc0 = std::make_shared<sc::Score>("x", 44100);
        sc0->insert(note(0.5, 0.1, 60));
        give(r, "s", sc0);
        r.run(start);                                  // play lands then
        r.box("tr")->message(0, {Atom("loop"), Atom(1.f), Atom(2.f)});   // bar 1: a second, again and again
        r.box("tr")->message(0, {Atom("locate"), Atom(1.f)});
        r.box("tr")->message(0, {Atom("play")});
        r.printed.clear();
        int beats = 0, notes = 0;
        for (int k = 0; k < 40 && beats <= 18; ++k) {   // (stalled, the same beat came again and again: given up)
            r.printed.clear();
            r.run(k < 39 ? 4410 : 100);
            for (auto& p : r.printed) {
                if (p.rfind("beat:", 0) == 0) ++beats;
                if (p.rfind("note:", 0) == 0) ++notes;
            }
        }
        if (beats < 15 || beats > 17 || notes != 4) {
            ++bad;
            std::printf("  start %ld: %d beats, %d notes\n", start, beats, notes);
        }
    }
    CHECK(bad == 0);
}

// The score replaced whole (clear, read, a score sent) forgets the selection, whose ids named the old one's events: a
// transformation then changes all of the new score; one whose selection is gone does too
TEST(score_replaced_forgets_its_selection) {
    Rig r(kPatch);
    auto s = std::make_shared<sc::Score>("x", 44100);
    s->insert(note(0, 0.5, 60));
    s->insert(note(0.5, 0.5, 62));
    give(r, "s", s);
    Box* b = r.box("s");
    b->message(0, {Atom("select"), Atom("all")});
    b->message(0, {Atom("clear")});
    b->message(0, {Atom("add"), Atom(0.f), Atom(0.5f), Atom("A"), Atom(64.f)});
    b->message(0, {Atom("add"), Atom(1.f), Atom(0.5f), Atom("A"), Atom(65.f)});
    b->message(0, {Atom("transpose"), Atom(2.f)});
    auto now = scoreOfBox(b);
    std::vector<double> pitches;
    for (auto* e : now->byTime()) pitches.push_back(e->pitch);
    CHECK(pitches.size() == 2 && pitches[0] == 66 && pitches[1] == 67);
    // a score sent with the same ids as the one selected: all of it transposed, not the ids that were selected
    b->message(0, {Atom("select"), Atom((float)now->byTime()[0]->id)});
    auto t = std::make_shared<sc::Score>("x", 44100);
    t->insert(note(0, 0.5, 60));
    t->insert(note(0.5, 0.5, 62));
    give(r, "s", t);
    b->message(0, {Atom("transpose"), Atom(1.f)});
    pitches.clear();
    for (auto* e : scoreOfBox(b)->byTime()) pitches.push_back(e->pitch);
    CHECK(pitches.size() == 2 && pitches[0] == 61 && pitches[1] == 63);
}

// A stereo sound on two outlets is panned too (a constant-power balance by its clip's and its track's azimuth), as a
// mono one is: to the left, the right channel silent and the left louder; in front, as it is
TEST(score_stereo_sound_panned) {
    Rig r("(patch (obj b buffer~ st 100) (obj s score x 2) (obj o dac~ 1 2) (connect s 0 o 0) (connect s 1 o 1)"
          " (obj tr transport 240 4 4))");
    auto* bb = dynamic_cast<BufferBox*>(r.box("b"));
    SampleBuffer st;
    st.channels = 2;
    st.sr = r.sr;
    st.frames = 4410;
    st.data.assign((size_t)st.frames * 2, 0.5f);
    bb->replace(std::move(st));
    auto play = [&](double az) {
        auto s = std::make_shared<sc::Score>("x", 44100);
        auto e = sound(0, 0.05, "st");
        e->az = az;
        s->insert(e);
        give(r, "s", s);
        r.box("tr")->message(0, {Atom("locate"), Atom(1.f)});
        r.box("tr")->message(0, {Atom("play")});
        auto y = r.run(441);
        r.box("tr")->message(0, {Atom("stop")});
        float L = etest::channel(y, 2, 0)[220], R = etest::channel(y, 2, 1)[220];
        return std::make_pair(L, R);
    };
    auto front = play(0);
    CHECK(std::fabs(front.first - 0.5f) < 1e-4 && std::fabs(front.second - 0.5f) < 1e-4);
    auto left = play(90);
    CHECK(std::fabs(left.first - 0.5f * std::sqrt(2.f)) < 1e-3 && std::fabs(left.second) < 1e-4);
    auto right = play(-45);
    CHECK(right.second > right.first && right.first > 0.1f);
    // the track's azimuth too
    auto s = std::make_shared<sc::Score>("x", 44100);
    auto e = sound(0, 0.05, "st");
    e->row = "A";
    s->insert(e);
    sc::Row row{"A", "", {}};
    sc::setField(row.extra, "az", sc::Datum::number(90));
    s->rows.push_back(row);
    give(r, "s", s);
    r.box("tr")->message(0, {Atom("locate"), Atom(1.f)});
    r.box("tr")->message(0, {Atom("play")});
    auto y = r.run(441);
    CHECK(std::fabs(etest::channel(y, 2, 1)[220]) < 1e-4);
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

// add sound at dur file: the file was quoted in the duration's place ("duration: a number is wanted"), so no
// sound could be added by a message
TEST(score_add_sound_message) {
    Rig r("(patch (obj s score piece 2))");
    r.box("s")->message(0, {Atom("add"), Atom("sound"), Atom(30.f), Atom(8.f), Atom("../../data/A_minor.wav")});
    CHECK(!r.printedSomething("score"));
    std::string text = r.p.serialize();
    CHECK(text.find("(sound 30 8 \\\"../../data/A_minor.wav\\\"") != std::string::npos);
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

// One transport for each top document: two patches open (extras of the root, as the windows' documents are) play and
// stop on their own; a subpatch follows its document's; the root's own is another still
TEST(transport_one_per_top_document) {
    Rig r("(patch (obj b buffer~ imp 100))");
    impulseBuffer(r, "b");
    auto open = [&](const char* text) {
        Patch* q = r.p.addExtra(std::make_unique<Patch>());
        std::string err;
        CHECK(q->load(text, err));
        q->loadbangAll();
        return q;
    };
    Patch* a = open("(patch (obj s score x 1) (obj o dac~ 1 2) (connect s 0 o 0) (obj tr transport 240 4 4)"
                    " (obj sub p inner (patch (obj t2 transport))))");
    Patch* b = open("(patch (obj s score y 1) (obj o dac~ 1 2) (connect s 0 o 1) (obj tr transport 240 4 4))");
    auto sa = std::make_shared<sc::Score>("x", 44100), sb = std::make_shared<sc::Score>("y", 44100);
    sa->insert(sound(0.25, 0.1, "imp"));
    sb->insert(sound(0.5, 0.1, "imp"));
    a->find("s")->message(0, {Atom("score"), Atom(shareScore(sa))});
    b->find("s")->message(0, {Atom("score"), Atom(shareScore(sb))});
    Patch* inner = a->find("sub") ? a->find("sub")->inner() : nullptr;
    CHECK(inner && inner->topDocument() == a && a->topDocument() == a && b->topDocument() == b);
    if (inner) CHECK(transportOf(inner) == transportOf(a));           // a subpatch: its document's
    CHECK(transportOf(a) != transportOf(b) && transportOf(a) != transportOf(&r.p) && transportOf(b) != transportOf(&r.p));
    a->find("tr")->message(0, {Atom("play")});                        // a's transport: only a plays
    auto y = r.run(44100);
    CHECK(transportOf(a)->playing() && !transportOf(b)->playing());
    auto L = peaks(etest::channel(y, 2, 0)), R = peaks(etest::channel(y, 2, 1));
    CHECK(L.size() == 1 && L[0] == 11025 && R.empty());
    a->find("tr")->message(0, {Atom("stop")});
    b->find("s")->message(0, {Atom("play")});                         // b's score: only b plays
    y = r.run(44100);
    L = peaks(etest::channel(y, 2, 0));
    R = peaks(etest::channel(y, 2, 1));
    CHECK(L.empty() && R.size() == 1 && R[0] == 22050);
    CHECK(!transportOf(a)->playing());
    r.p.removeExtra(a);
    r.p.removeExtra(b);
    r.p.collectGarbage(true);
}

// Play sent to a score box from the patch: from where the transport is; at the score's end or past it, from the start;
// a play sent there stops at the score's end (the patch's clock started elsewhere goes on); a play while it plays the
// score changes nothing; stop and play again goes on from where it stopped; looping, it goes on
TEST(score_play_from_the_patch) {
    Rig r(kPatch);
    impulseBuffer(r, "b");
    auto s = std::make_shared<sc::Score>("x", 44100);
    s->insert(sound(0.25, 0.1, "imp"));
    s->insert(note(0.5, 0.1, 60));                                    // the score ends at 0.6 s
    give(r, "s", s);
    auto t = transportOf(&r.p);
    std::vector<long> snd, clk;
    auto play = [&](long frames) {
        auto y = r.run(frames);
        snd = peaks(etest::channel(y, 2, 0));
        clk = peaks(etest::channel(y, 2, 1));
    };
    r.box("s")->message(0, {Atom("play")});
    play(44100);
    CHECK(snd.size() == 1 && snd[0] == 11025 && clk.size() == 1 && clk[0] == 22050);
    CHECK(!t->playing());                                             // stopped at its end, on its sample
    int lap;
    double pos;
    t->where(&r.p, lap, pos);
    CHECK(std::fabs(pos - 0.6 * 44100) < 1);
    r.box("s")->message(0, {Atom("play")});                           // at the end: from the start, again
    play(44100);
    CHECK(snd.size() == 1 && snd[0] == 11025 && clk.size() == 1 && clk[0] == 22050 && !t->playing());
    // the transport object starts the patch's clock (past the score's end: nothing, and it goes on)
    r.box("tr")->message(0, {Atom("play")});
    play(44100);
    CHECK(snd.empty() && clk.empty() && t->playing());
    r.box("s")->message(0, {Atom("play")});                           // running past the end: from the start, and stops
    play(44100);
    CHECK(snd.size() == 1 && snd[0] == 11025 && clk.size() == 1 && !t->playing());
    // the transport object from the start: the score plays, the clock goes on past its end
    r.box("tr")->message(0, {Atom("rewind")});
    r.box("tr")->message(0, {Atom("play")});
    play(44100);
    CHECK(snd.size() == 1 && t->playing());
    r.box("tr")->message(0, {Atom("stop")});
    // play while it plays the score: nothing changes; stop, then play goes on from there
    r.box("tr")->message(0, {Atom("rewind")});
    r.box("s")->message(0, {Atom("play")});
    play(4410);                                                       // 0.1 s
    r.box("s")->message(0, {Atom("play")});
    play(4410);                                                       // 0.2 s
    r.box("s")->message(0, {Atom("stop")});
    play(4410);
    CHECK(snd.empty() && !t->playing());
    r.box("s")->message(0, {Atom("play")});
    play(44100);
    CHECK(snd.size() == 1 && snd[0] == 2205 && clk.size() == 1 && clk[0] == 13230 && !t->playing());
    // looping (bar 1, a second at 240 bpm): it goes on past the score's end, round the loop
    r.box("tr")->message(0, {Atom("loop"), Atom(1.f), Atom(2.f)});
    r.box("tr")->message(0, {Atom("rewind")});
    r.box("s")->message(0, {Atom("play")});
    play(2 * 44100);
    CHECK(snd.size() == 2 && snd[0] == 11025 && snd[1] == 44100 + 11025 && t->playing());
    r.box("tr")->message(0, {Atom("stop")});
    CHECK(!r.printedSomething("score:"));
}

// Audition (a click on a note in a roll): a note with a sample of its own plays it through the preview voice at its
// pitch (an octave above the sample's: twice as fast) for its duration, faded in and out; a note without one leaves the
// notes outlet as playing sends it and, at its end, as a note-off (velocity 0); with nothing connected there, a tone
// through the preview; the transport is never started
TEST(score_audition) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("moritz_audition_" + std::to_string(std::rand()));
    fs::create_directories(dir);
    SampleBuffer ramp;                               // sample k is k / N: the frame read is seen in the output
    ramp.channels = 1;
    ramp.sr = 44100;
    ramp.frames = 44100;
    ramp.data.resize(44100);
    for (long k = 0; k < ramp.frames; ++k) ramp.data[(size_t)k] = (float)k / (float)ramp.frames;
    std::string err;
    CHECK(writeWavFile((dir / "ramp.wav").string(), ramp, err, 24));
    Rig r("(patch (obj s score x 1) (obj p print n) (obj o dac~ 1) (connect s 1 p 0) (connect s 0 o 0) (obj q score y 1))");
    r.p.dir = dir.string();
    auto t = transportOf(&r.p);
    sc::Event e = *note(0, 0.1, 72);
    sc::setField(e.extra, "sound", sc::Datum::string("ramp.wav"));
    sc::setField(e.extra, "sound-pitch", sc::Datum::number(60));
    PreviewPlayer& pv = App::get().audio.preview();
    std::string said = auditionInBox(r.box("s"), e, nullptr);
    CHECK(said.find("the note's sample") != std::string::npos && said.find("audio is off") != std::string::npos);
    std::vector<float> out(8192, 0.f);
    pv.render(out.data(), 1, (int)out.size(), 44100);
    CHECK(std::fabs(out[1000] - 2000.f / 44100.f) < 1e-3f);          // twice as fast, past the fade-in
    CHECK(out[100] < 200.f / 44100.f);                               // fading in
    CHECK(std::fabs(out[4000] - 8000.f / 44100.f) < 1e-3f && out[4500] == 0.f);   // a tenth of a second, then nothing
    // without a sample: out of the notes outlet, then its note-off at its end
    sc::Event plain = *note(0, 0.25, 64);
    said = auditionInBox(r.box("s"), plain, nullptr);
    CHECK(said.find("notes outlet") != std::string::npos);
    CHECK(r.printed.size() == 1 && r.printed[0].rfind("n: 64 ", 0) == 0 && r.printed[0].find(" 250 ") != std::string::npos);
    r.run(4410 * 2);
    CHECK(r.printed.size() == 1);
    r.run(4410 * 2);
    CHECK(r.printed.size() == 2 && r.printed[1].rfind("n: 64 0 0", 0) == 0);
    CHECK(!t->playing());
    // nothing connected to the notes outlet: a tone, through the preview
    said = auditionInBox(r.box("q"), plain, nullptr);
    CHECK(said.find("as a tone") != std::string::npos);
    std::fill(out.begin(), out.end(), 0.f);
    pv.render(out.data(), 1, (int)out.size(), 44100);
    float peak = 0;
    for (float x : out) peak = std::max(peak, std::fabs(x));
    CHECK(peak > 0.05f);
    CHECK(auditionInBox(r.box("p"), plain, nullptr).empty());          // not a score box
    pv.stop();
    std::fill(out.begin(), out.end(), 0.f);
    pv.render(out.data(), 1, 512, 44100);
    pv.collect();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// A score written to another folder keeps what it places: its relative references (a score placed by reference, a
// sound file, a note's own sample, the same inside a score placed by value) are named from the new file's folder (near:
// relative, far: absolute), so the file reads back whole, from the score object's write, a document's Save As and
// export, and Musil's writer; the score object's errors are said once ("score: cannot open", not "score: score: ...")
TEST(score_written_elsewhere_keeps_its_references) {
    namespace fs = std::filesystem;
    fs::path T = fs::weakly_canonical(fs::temp_directory_path()) / ("moritz_refs_" + std::to_string(std::rand()));
    fs::create_directories(T / "lib");
    fs::create_directories(T / "tut");
    SampleBuffer b;
    b.channels = 1;
    b.sr = 44100;
    b.frames = 100;
    b.data.assign(100, 0.5f);
    std::string err;
    CHECK(writeWavFile((T / "lib" / "snd.wav").string(), b, err));
    {
        std::ofstream m(T / "lib" / "motif.score");
        m << "(score \"motif\" (note 0 0.5 Fl 60) (note 0.5 0.5 Fl 62))";
    }
    // the patch's score names them from the patch's folder
    std::string text =
        "(score \"tune\" (note 0 1 Fl 67 mf (data sound \"../lib/snd.wav\"))"
        " (score 1 \"../lib/motif.score\" (row Fl))"
        " (sound 2 0.5 \"../lib/snd.wav\" (row drums))"
        " (score 3 (score \"inner\" (sound 0 0.5 \"../lib/snd.wav\")) (dur 1) (row drums)))";
    Rig r("");
    r.p.dir = (T / "tut").string();
    CHECK(r.p.load("(patch (obj s score tune 1) (obj p print w))", err));
    give(r, "s", sc::readText(text, (T / "tut" / "score s").string()));   // as kept in the patch: named from its folder
    auto check = [&](const fs::path& file, const char* what) {
        std::shared_ptr<sc::Score> back;
        try { back = sc::readFile(file.string()); } catch (const std::exception& e) { std::printf("  %s: %s\n", what, e.what()); }
        CHECK(back != nullptr);
        if (!back) return;
        std::string dir = sc::folderOf(file.string());
        int found = 0;
        for (auto& e : back->events()) {
            if (e->kind == sc::Kind::Score && !e->ref.empty()) found += e->sub && e->sub->size() == 2;
            if (e->kind == sc::Kind::Sound) found += !sc::resolveSource(e->source, dir, {}).empty();
            if (e->kind == sc::Kind::Note) found += !sc::resolveSource(sc::field(e->extra, "sound")->s, dir, {}).empty();
            if (e->kind == sc::Kind::Score && e->ref.empty() && e->sub && e->sub->size() == 1)
                found += !sc::resolveSource(e->sub->events()[0]->source, dir, {}).empty();
        }
        if (found != 4) std::printf("  %s: %d of 4 found\n", what, found);
        CHECK(found == 4);
    };
    // the score object's write: below the patch's folder (relative), and far below it (absolute)
    fs::create_directories(T / "tut" / "out");
    fs::create_directories(T / "tut" / "a" / "b" / "c");
    r.box("s")->message(0, {Atom("write"), Atom((T / "tut" / "out" / "w.score").string())});
    CHECK(!r.printedSomething("score:"));
    check(T / "tut" / "out" / "w.score", "write");
    std::string written;
    {
        std::ifstream f(T / "tut" / "out" / "w.score");
        written.assign(std::istreambuf_iterator<char>(f), {});
    }
    CHECK(written.find("\"../../lib/motif.score\"") != std::string::npos);
    r.box("s")->message(0, {Atom("read"), Atom((T / "tut" / "out" / "w.score").string())});   // and read back: whole
    CHECK(!r.printedSomething("score:"));
    CHECK(scoreOfBox(r.box("s"))->size() == 4);
    r.box("s")->message(0, {Atom("write"), Atom((T / "tut" / "a" / "b" / "c" / "far.score").string())});
    check(T / "tut" / "a" / "b" / "c" / "far.score", "write far");
    // Musil's writer: named from the file it was read from
    auto s = sc::readFile((T / "tut" / "out" / "w.score").string());
    sc::writeFile(*s, (T / "lib" / "m.score").string());
    check(T / "lib" / "m.score", "musil");
    // a document's Save As and its export
    std::string e2;
    auto d = ScoreDoc::open((T / "tut" / "out" / "w.score").string(), e2);
    CHECK(d != nullptr);
    if (d) {
        CHECK(d->exportTo((T / "copy.score").string(), e2));
        check(T / "copy.score", "export");
        CHECK(d->saveAs((T / "lib" / "saved.score").string(), e2));
        check(T / "lib" / "saved.score", "save as");
        for (auto& e : d->score().events())                 // the document is that file's now: named from there
            if (e->kind == sc::Kind::Score && !e->ref.empty()) CHECK(e->ref == "motif.score");
        CHECK(d->save(e2));
        check(T / "lib" / "saved.score", "save");
    }
    // a read that fails is said once
    r.printed.clear();
    r.box("s")->message(0, {Atom("read"), Atom("nothing-here.score")});
    CHECK(r.printedSomething("score: cannot open") && !r.printedSomething("score: score:"));
    std::error_code ec;
    fs::remove_all(T, ec);
}
