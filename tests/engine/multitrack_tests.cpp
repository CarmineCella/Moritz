// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// multitrack_tests.cpp — the multitrack mode (multitrack.h, multitrack_ops.h): the strips' values through atomics (a
// fader moved makes no plan), buses and sends, tracks on several cores equal to one thread bit for bit, crossfades and
// fade shapes, strip lanes, inserts, instruments and patch clips, long files read from disk (as decoded, with a slow
// disk and seeks), recording with a simulated device (exact to the sample once the latency is taken off, punch-in,
// count-in), stems, and the threaded paths (the audio thread running while the interface records and changes things).

#include "rig.h"
#include "multitrack.h"
#include "multitrack_ops.h"
#include "score_doc.h"
#include "timeline_ops.h"
#include "transport.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

using namespace moritz;
using etest::Rig;
namespace tl = moritz::timeline;
namespace fs = std::filesystem;

namespace {

const char* kMt =
    "(patch (obj k buffer~ one 100) (obj b buffer~ imp 100) (obj s score x 2) (obj o dac~ 1 2) (obj tr transport 120 4 4)"
    " (connect s 0 o 0) (connect s 1 o 1))";
const float kHalf = (float)std::sqrt(0.5);   // a mono sound in the middle of two channels

void fillBuffer(Rig& r, const char* id, long frames, const std::function<float(long)>& f) {
    auto* b = dynamic_cast<BufferBox*>(r.box(id));
    SampleBuffer s;
    s.channels = 1;
    s.sr = r.sr;
    s.frames = frames;
    s.data.resize((size_t)frames);
    for (long i = 0; i < frames; ++i) s.data[(size_t)i] = f(i);
    b->replace(std::move(s));
}
fs::path scratch(const char* name) {
    fs::path dir = fs::temp_directory_path() / (std::string("moritz_mt_") + name + "_" + std::to_string(std::rand()));
    fs::create_directories(dir);
    return dir;
}
void writeText(const fs::path& p, const std::string& t) {
    std::ofstream f(p);
    f << t;
}
// the transport played from a position for some frames, then stopped
std::vector<float> playFor(Rig& r, double from, long frames) {
    r.box("tr")->message(0, {Atom("seek"), Atom((float)(from * 1000))});
    r.box("tr")->message(0, {Atom("play")});
    auto y = r.run(frames);
    r.box("tr")->message(0, {Atom("stop")});
    return y;
}
bool near(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; }

}  // namespace

// A fader, a pan, mute and the master reach the audio thread through the strips' atomics: no new plan for them, from
// the mixer (the atomic itself) or from the score (a strip edited: its values only)
TEST(multitrack_mixer_values_through_atomics) {
    Rig r(kMt);
    fillBuffer(r, "k", 44100 * 4, [](long) { return 1.f; });
    Box* s = r.box("s");
    auto d = ScoreDoc::ofBox(s);
    tl::addTrack(*d, "A");
    tl::addSound(*d, "one", true, "A", 0, 3);
    uint64_t plans = mt::planInfo(s).plans;
    auto y = playFor(r, 0, 4410);
    CHECK(near(y[2 * 4000], kHalf) && near(y[2 * 4000 + 1], kHalf));
    mt::StripParams* a = mt::stripOf(s, "A");
    CHECK(a != nullptr);
    a->gain.store(0.5f);                                  // the fader dragged
    y = playFor(r, 0.5, 4410);
    CHECK(near(y[2 * 4000], 0.5f * kHalf));
    sc::Strip st;
    st.gain = 0.25;                                       // the strip edited in the score: one undo step, no plan
    tl::setStrip(*d, "A", st);
    CHECK(a->gain.load() == 0.25f);
    y = playFor(r, 0.5, 4410);
    CHECK(near(y[2 * 4000], 0.25f * kHalf));
    a->az.store(90.f);                                    // hard left: a constant-power balance
    y = playFor(r, 0.5, 4410);
    CHECK(near(y[2 * 4000], 0.25f) && near(y[2 * 4000 + 1], 0.f));
    a->az.store(0.f);
    st.mute = true;
    tl::setStrip(*d, "A", st);
    y = playFor(r, 0.5, 4410);
    CHECK(etest::peakOf(y, 2 * 200) < 1e-6f);             // after its ramp (one block)
    st.mute = false;
    tl::setStrip(*d, "A", st);
    mt::stripOf(s, mt::kMaster)->gain.store(0.f);
    y = playFor(r, 0.5, 4410);
    CHECK(etest::peakOf(y, 2 * 200) < 1e-6f);
    mt::stripOf(s, mt::kMaster)->gain.store(1.f);
    y = playFor(r, 0.5, 4410);
    CHECK(a->takePeak(0) > 0.1f && a->rms[0].load() > 0.f);   // the meters
    CHECK(mt::planInfo(s).plans == plans);                // none of that made a plan
    d->undo();                                            // back to the strip before (still no plan)
    CHECK(mt::planInfo(s).plans == plans);
    tl::addSound(*d, "one", true, "A", 3, 0.5);           // a clip added: a plan
    CHECK(mt::planInfo(s).plans == plans + 1);
}

// Buses: a track goes to a bus (its output), sends to another (post-fader); the buses go to the master; a loop of buses
// is broken and said
TEST(multitrack_buses_and_sends) {
    Rig r(kMt);
    fillBuffer(r, "k", 44100 * 4, [](long) { return 1.f; });
    Box* s = r.box("s");
    auto d = ScoreDoc::ofBox(s);
    tl::addTrack(*d, "A");
    tl::addSound(*d, "one", true, "A", 0, 3);
    CHECK(mt::addBus(*d, "fx") == "fx");
    CHECK(mt::addBus(*d, "verb") == "verb");
    CHECK(mt::setOutput(*d, "A", "fx"));
    sc::Strip half;
    half.gain = 0.5;
    tl::setStrip(*d, "fx", half);
    CHECK(mt::setSend(*d, "A", "verb", 0.5));
    auto info = mt::planInfo(s);
    CHECK(info.tracks == 1 && info.buses == 2);
    auto y = playFor(r, 0, 4410);
    CHECK(near(y[2 * 4000], 0.5f * kHalf + 0.5f * kHalf));
    mt::stripOf(s, "A")->send[0].store(0.f);              // the send's level: an atomic
    y = playFor(r, 0.5, 4410);
    CHECK(near(y[2 * 4000], 0.5f * kHalf));
    CHECK(!mt::setSend(*d, "A", "nowhere", 1));
    // fx into verb and verb into fx: a loop, broken
    CHECK(mt::setOutput(*d, "fx", "verb"));
    CHECK(mt::setOutput(*d, "verb", "fx"));
    CHECK(r.printedSomething("loop"));
    y = playFor(r, 0.5, 4410);
    CHECK(std::isfinite(y[2 * 4000]));
}

// The tracks run on several cores give the same output as one thread, bit for bit (each writes only its own buffer; the
// sums are made in order on the audio thread), with inserts and buses among them
TEST(multitrack_parallel_equals_one_thread) {
    fs::path dir = scratch("par");
    writeText(dir / "halve.moritz",
              "(patch (obj i inlet~ 1) (obj j inlet~ 2) (obj a *~ 0.5) (obj b *~ 0.5) (obj o outlet~ 1) (obj p outlet~ 2)"
              " (connect i 0 a 0) (connect j 0 b 0) (connect a 0 o 0) (connect b 0 p 0))");
    Rig r(kMt);
    std::mt19937 rng(7);
    std::vector<float> noise(44100 * 3);
    for (auto& v : noise) v = (float)((double)rng() / rng.max() * 2 - 1) * 0.3f;
    fillBuffer(r, "k", (long)noise.size(), [&](long i) { return noise[(size_t)i]; });
    fillBuffer(r, "b", 44100, [](long i) { return i % 100 == 0 ? 0.5f : 0.f; });
    Box* s = r.box("s");
    auto d = ScoreDoc::ofBox(s);
    mt::addBus(*d, "fx");
    mt::SessionSettings xf;
    xf.crossfade = true;
    mt::setSession(*d, xf, "Crossfades");
    for (int k = 0; k < 16; ++k) {
        std::string t = "t" + std::to_string(k);
        tl::addTrack(*d, t);
        uint64_t id = tl::addSound(*d, k % 2 ? "one" : "imp", true, t, 0.01 * k, 1.2);
        mt::setClipFades(*d, id, 0.05 * (k % 3), 0.1, k % 4 == 0 ? "equal" : "", "slow");
        tl::addSound(*d, "one", true, t, 1.0 + 0.01 * k, 0.6);   // overlapping: a crossfade
        sc::Strip st;
        st.gain = 0.2 + 0.05 * k;
        st.az = -80 + 10 * k;
        tl::setStrip(*d, t, st);
        if (k % 4 == 1) mt::setTrackPatch(*d, t, "insert", (dir / "halve.moritz").string());
        if (k % 5 == 2) mt::setSend(*d, t, "fx", 0.3);
    }
    CHECK(mt::planInfo(s).slots == 4);
    auto render = [&](int threads) {
        mt::TrackPool::threads.store(threads);
        int minT = mt::TrackPool::minTracks.load();
        mt::TrackPool::minTracks.store(2);
        auto y = playFor(r, 0, 44100 * 2);
        mt::TrackPool::minTracks.store(minT);
        mt::TrackPool::threads.store(0);
        return y;
    };
    auto one = render(1);
    uint64_t helped = mt::TrackPool::get().helped(), jobs = mt::TrackPool::get().jobs();
    auto many = render(0);
    CHECK(one.size() == many.size() && one == many);
    CHECK(etest::peakOf(one) > 0.1f);
    if (mt::TrackPool::get().workers() > 0) {
        CHECK(mt::TrackPool::get().jobs() > jobs);
        std::printf("  the workers ran %llu of the tracks' blocks\n", (unsigned long long)(mt::TrackPool::get().helped() - helped));
    }
    long before = etest::audioAllocations();
    render(0);
    if (etest::allocGuardActive()) CHECK(etest::audioAllocations() == before);
    r.p.collectGarbage(true);
}

// What 16 tracks cost (printed): one thread and several, per second of audio, with the streams and inserts of a session
TEST(multitrack_cpu_sixteen_tracks) {
    fs::path dir = scratch("cpu");
    // an insert as a session's would be: twenty filters in a row on each side
    std::string lp = "(patch (obj i inlet~ 1) (obj j inlet~ 2) (obj o outlet~ 1) (obj p outlet~ 2)";
    for (int k = 0; k < 20; ++k) {
        std::string a = "a" + std::to_string(k), b = "b" + std::to_string(k);
        lp += " (obj " + a + " lores~ " + std::to_string(800 + 150 * k) + " 0.3) (obj " + b + " lores~ " + std::to_string(900 + 150 * k) + " 0.3)";
        lp += k == 0 ? " (connect i 0 a0 0) (connect j 0 b0 0)"
                     : " (connect a" + std::to_string(k - 1) + " 0 " + a + " 0) (connect b" + std::to_string(k - 1) + " 0 " + b + " 0)";
    }
    lp += " (connect a19 0 o 0) (connect b19 0 p 0))";
    writeText(dir / "lp.moritz", lp);
    Rig r(kMt, 48000);
    std::mt19937 rng(3);
    fillBuffer(r, "k", 48000 * 12, [&](long) { return (float)((double)rng() / rng.max() * 2 - 1) * 0.1f; });
    Box* s = r.box("s");
    auto d = ScoreDoc::ofBox(s);
    for (int k = 0; k < 16; ++k) {
        std::string t = "t" + std::to_string(k);
        tl::addTrack(*d, t);
        for (int c = 0; c < 6; ++c) tl::addSound(*d, "one", true, t, 1.9 * c + 0.01 * k, 2.0);
        mt::setTrackPatch(*d, t, "insert", (dir / "lp.moritz").string());
    }
    auto cost = [&](int threads) {
        mt::TrackPool::threads.store(threads);
        r.box("tr")->message(0, {Atom("seek"), Atom(0.f)});
        r.box("tr")->message(0, {Atom("play")});
        r.run(4800);
        auto t0 = std::chrono::steady_clock::now();
        r.run(48000 * 4, 256);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        r.box("tr")->message(0, {Atom("stop")});
        mt::TrackPool::threads.store(0);
        return sec / 4 * 100;                              // percent of real time
    };
    double one = cost(1), many = cost(0);
    std::printf("  16 tracks (6 clips and an insert of 40 filters each), 48 kHz, 256-frame callbacks: %.1f%% of real time on one "
                "thread, %.1f%% on %d (the audio thread and %d workers)\n",
                one, many, mt::TrackPool::get().workers() + 1, mt::TrackPool::get().workers());
    CHECK(one > 0 && many > 0);
}

// Clips overlapping on a track crossfade (equal power) over the overlap; fades have shapes
TEST(multitrack_crossfades_and_fade_shapes) {
    Rig r(kMt);
    fillBuffer(r, "k", 44100 * 4, [](long) { return 1.f; });
    Box* s = r.box("s");
    auto d = ScoreDoc::ofBox(s);
    tl::addTrack(*d, "A");
    mt::SessionSettings ss;
    ss.crossfade = true;                                  // a session's default
    CHECK(mt::setSession(*d, ss, "Crossfades"));
    uint64_t first = tl::addSound(*d, "one", true, "A", 0, 1.0);
    uint64_t second = tl::addSound(*d, "one", true, "A", 0.5, 1.0);
    // the overlap's crossfade written into the clips (one undo step with the clip added)
    CHECK(sc::numField(d->score().find(first)->extra, "fade-out", 0) == 0.5);
    CHECK(sc::numField(d->score().find(second)->extra, "fade-in", 0) == 0.5);
    auto y = playFor(r, 0, 44100 * 2);
    auto L = etest::channel(y, 2, 0);
    // in the overlap: cos and sin of a quarter turn, their sum about 1.41 at its middle (equal power)
    CHECK(near(L[22050 + 11025], kHalf * (std::sin(0.49995f * 1.5707963f) + std::sin(0.5f * 1.5707963f)), 1e-3f));
    CHECK(near(L[11025], kHalf) && near(L[66150 - 2000], kHalf));
    // the shapes of a fade-in: equal power, slow (squared), fast (square root), linear
    tl::deleteTrack(*d, "A");
    tl::addTrack(*d, "B");
    uint64_t id = tl::addSound(*d, "one", true, "B", 0, 1.0);
    const char* shapes[] = {"equal", "slow", "fast", "linear"};
    float want[] = {std::sin(0.25f * 3.14159265f), 0.25f, std::sqrt(0.5f), 0.5f};
    for (int k = 0; k < 4; ++k) {
        CHECK(mt::setClipFades(*d, id, 0.1, 0, shapes[k]));
        y = playFor(r, 0, 4410);
        L = etest::channel(y, 2, 0);
        CHECK(near(L[2205], kHalf * want[k], 1e-3f));
    }
    CHECK(mt::setClipGain(*d, id, 0.5));
    y = playFor(r, 0, 44100);
    CHECK(near(etest::channel(y, 2, 0)[30000], 0.5f * kHalf));
}

// A lane named strip.gain (dB) drives its track's fader, ramping between its points, on the audio thread; strip.mute
// mutes it; neither goes to a receive
TEST(multitrack_strip_lanes) {
    Rig r(std::string(kMt).substr(0, std::string(kMt).size() - 1) + " (obj rv r strip.gain) (obj pr print got))");
    fillBuffer(r, "k", 44100 * 4, [](long) { return 1.f; });
    Box* s = r.box("s");
    auto d = ScoreDoc::ofBox(s);
    tl::addTrack(*d, "A");
    tl::addSound(*d, "one", true, "A", 0, 3);
    tl::addLane(*d, "A", "strip.gain", -60, 6);
    tl::addPoint(*d, "A", "strip.gain", 0, -60);
    tl::addPoint(*d, "A", "strip.gain", 1, 0);
    auto y = playFor(r, 0, 44100 * 2);
    auto L = etest::channel(y, 2, 0);
    CHECK(std::fabs(L[22050] / kHalf - (float)mt::dbToGain(-30)) < 0.002f);
    CHECK(near(L[60000], kHalf));
    CHECK(L[11025] < L[22050] && L[22050] < L[33075]);
    tl::addLane(*d, "A", "strip.mute", 0, 1);
    tl::addPoint(*d, "A", "strip.mute", 0, 0);           // before its first point a lane holds that point's value
    tl::addPoint(*d, "A", "strip.mute", 1.5, 1);
    y = playFor(r, 0, 44100 * 2);
    L = etest::channel(y, 2, 0);
    CHECK(near(L[60000], kHalf) && L[70000] == 0.f);
    CHECK(!r.printedSomething("got"));
}

// A track's insert (a patch from inlet~ to outlet~), its instrument (a patch playing its notes) and a patch clip (run
// while it plays, loadbang at its start)
TEST(multitrack_track_patches) {
    fs::path dir = scratch("patches");
    writeText(dir / "halve.moritz",
              "(patch (obj i inlet~ 1) (obj j inlet~ 2) (obj a *~ 0.5) (obj b *~ 0.5) (obj o outlet~ 1) (obj p outlet~ 2)"
              " (connect i 0 a 0) (connect j 0 b 0) (connect a 0 o 0) (connect b 0 p 0))");
    writeText(dir / "clicker.moritz",
              "(patch (obj i inlet) (obj t t b) (obj c click~) (obj o outlet~ 1) (obj p print instrument)"
              " (connect i 0 t 0) (connect t 0 c 0) (connect c 0 o 0) (connect i 0 p 0))");
    writeText(dir / "steady.moritz",
              "(patch (obj s sig~ 0.25) (obj o outlet~ 1) (obj l loadbang) (obj p print clip) (connect s 0 o 0) (connect l 0 p 0))");
    Rig r(std::string(kMt).substr(0, std::string(kMt).size() - 1) + " (obj np print notes) (connect s 2 np 0))");
    fillBuffer(r, "k", 44100 * 4, [](long) { return 1.f; });
    Box* s = r.box("s");
    auto d = ScoreDoc::ofBox(s);
    tl::addTrack(*d, "A");
    tl::addSound(*d, "one", true, "A", 0, 3);
    CHECK(mt::setTrackPatch(*d, "A", "insert", (dir / "halve.moritz").string()));
    CHECK(mt::trackPatch(s, "A", "insert") != nullptr);
    auto y = playFor(r, 0, 4410);
    CHECK(near(y[2 * 4000], 0.5f * kHalf) && near(y[2 * 4000 + 1], 0.5f * kHalf));
    CHECK(mt::setTrackPatch(*d, "A", "insert", ""));
    CHECK(mt::trackPatch(s, "A", "insert") == nullptr);
    y = playFor(r, 0, 4410);
    CHECK(near(y[2 * 4000], kHalf));
    // an instrument plays the notes of its track (they leave the notes outlet no more)
    tl::deleteTrack(*d, "A");
    tl::addTrack(*d, "N");
    auto note = std::make_shared<sc::Event>();
    note->kind = sc::Kind::Note;
    note->at = 0.5;
    note->dur = 0.25;
    note->pitch = 60;
    note->row = "N";
    {
        auto e = d->edit("Note");
        e.insert(note);
        e.commit();
    }
    CHECK(mt::setTrackPatch(*d, "N", "instrument", (dir / "clicker.moritz").string()));
    y = playFor(r, 0, 44100);
    auto L = etest::channel(y, 2, 0);
    long hits = 0, at = -1;
    for (size_t i = 0; i < L.size(); ++i)
        if (L[i] > 0.5f) { ++hits; at = (long)i; }
    CHECK(hits == 1 && at == 22050);
    CHECK(r.printedSomething("instrument: note 60") && !r.printedSomething("notes:"));
    // a patch clip runs while it plays: from its start to its end, loadbang at its start
    tl::deleteTrack(*d, "N");
    CHECK(tl::addPatchClip(*d, (dir / "steady.moritz").string(), "P", 0.5, 0.5) != 0);
    r.printed.clear();
    y = playFor(r, 0, 4410);
    CHECK(!r.printedSomething("clip: bang"));
    y = playFor(r, 0, 44100 + 4410);
    L = etest::channel(y, 2, 0);
    CHECK(L[22049] == 0.f && near(L[22050], 0.25f) && near(L[44099], 0.25f) && L[44100] == 0.f);
    CHECK(r.printedSomething("clip: bang"));
    CHECK(mt::clipPatch(s, d->score().events().back()->id) != nullptr);
    r.p.collectGarbage(true);
}

// A long file is read from disk while it plays: the same samples as decoded whole; with a slow disk a locate is a few
// silent blocks (misses), then the same again; nothing allocated on the audio thread
TEST(multitrack_long_files_from_disk) {
    fs::path dir = scratch("stream");
    SampleBuffer src;
    src.channels = 2;
    src.sr = 44100;
    src.frames = 44100 * 4;
    src.data.resize((size_t)src.frames * 2);
    std::mt19937 rng(11);
    for (auto& v : src.data) v = (float)((double)rng() / rng.max() * 2 - 1) * 0.5f;
    std::string err, path = (dir / "long.wav").string();
    CHECK(writeWavFile(path, src, err, 24));
    auto renderWith = [&](double threshold, bool flush, double from, long frames) {
        double keep = mt::streamThreshold.load();
        mt::streamThreshold.store(threshold);
        Rig r(kMt);
        auto d = ScoreDoc::ofBox(r.box("s"));
        uint64_t id = tl::addSound(*d, path, false, "A", 0.25, 3.0);
        sc::Event e = *d->score().find(id);
        e.from = 0.5;
        e.to = 3.5;
        tl::change(*d, e, "From");
        int streamed = mt::planInfo(r.box("s")).streamed;
        r.box("tr")->message(0, {Atom("seek"), Atom((float)(from * 1000))});
        r.box("tr")->message(0, {Atom("play")});
        std::vector<float> all, buf(2 * 64);
        for (long done = 0; done < frames; done += 64) {
            if (flush) mt::DiskReader::get().flush();
            r.p.runDue((double)(r.frame + 64) / r.sr);
            r.p.process(nullptr, 0, buf.data(), 2, 64);
            all.insert(all.end(), buf.begin(), buf.end());
            r.frame += 64;
        }
        mt::streamThreshold.store(keep);
        return std::make_pair(all, streamed);
    };
    auto mem = renderWith(1e9, false, 0, 44100 * 3);
    auto disk = renderWith(1.0, true, 0, 44100 * 3);
    CHECK(mem.second == 0 && disk.second == 1);
    CHECK(mem.first == disk.first);
    CHECK(etest::peakOf(mem.first) > 0.2f);
    // a slow disk, and a locate into the middle: misses at first, then the samples as decoded
    mt::DiskStream::testDelayMs.store(15);
    long before = etest::audioAllocations();
    double keep = mt::streamThreshold.load();
    mt::streamThreshold.store(1.0);
    {
        Rig r(kMt);
        auto d = ScoreDoc::ofBox(r.box("s"));
        uint64_t id = tl::addSound(*d, path, false, "A", 0.25, 3.0);
        sc::Event e = *d->score().find(id);
        e.from = 0.5;
        e.to = 3.5;
        tl::change(*d, e, "From");
        r.box("tr")->message(0, {Atom("seek"), Atom(2000.f)});
        r.box("tr")->message(0, {Atom("play")});
        std::vector<float> buf(2 * 64), late;
        long missed = 0;
        for (int b = 0; b < 400; ++b) {
            r.p.runDue((double)(r.frame + 64) / r.sr);
            r.p.process(nullptr, 0, buf.data(), 2, 64);
            r.frame += 64;
            if (b < 4 && etest::peakOf(buf) == 0.f) ++missed;
            if (b >= 300) late.insert(late.end(), buf.begin(), buf.end());
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        // the reference at the same positions: 2 s + 300 blocks
        size_t at = (size_t)(88200 + 300 * 64) * 2;
        std::vector<float> ref(mem.first.begin() + (long)at, mem.first.begin() + (long)(at + late.size()));
        CHECK(missed > 0);
        CHECK(late == ref);
    }
    mt::streamThreshold.store(keep);
    mt::DiskStream::testDelayMs.store(0);
    if (etest::allocGuardActive()) CHECK(etest::audioAllocations() == before);
    // the overview of a long file: computed, cached next to it, read back while the file is the same
    mt::Peaks p, q;
    CHECK(mt::computePeaks(path, p, err) && p.frames == src.frames && p.channels == 2);
    CHECK(mt::savePeaks(path, p) && fs::exists(path + ".peaks"));
    CHECK(mt::loadPeaks(path, q) && q.mn == p.mn && q.mx == p.mx);
    float lo, hi;
    q.range(0, 0, 44100, lo, hi);
    CHECK(lo < -0.4f && hi > 0.4f);
    writeText(path + ".peaks", "garbage");
    CHECK(!mt::loadPeaks(path, q));
}

namespace {
// a device simulated: what goes out comes back in `latency` samples later (a cable from the output to the input);
// blocks of 64, the timers run before each
struct Loopback {
    Rig& r;
    int latency;
    std::vector<float> history;          // the left output, from frame 0
    explicit Loopback(Rig& rig, int lat) : r(rig), latency(lat) {}
    std::vector<float> run(long frames) {
        std::vector<float> out, in(64), buf(2 * 64);
        for (long done = 0; done < frames; done += 64) {
            for (int i = 0; i < 64; ++i) {
                long t = r.frame + i - latency;
                in[(size_t)i] = t >= 0 && t < (long)history.size() ? history[(size_t)t] : 0.f;
            }
            r.p.runDue((double)(r.frame + 64) / r.sr);
            r.p.process(in.data(), 1, buf.data(), 2, 64);
            for (int i = 0; i < 64; ++i) history.push_back(buf[(size_t)i * 2]);
            out.insert(out.end(), buf.begin(), buf.end());
            r.frame += 64;
        }
        return out;
    }
};
}  // namespace

// Recording into an armed track with a device that brings the output back `latency` samples late: the take, moved by
// the latency, lines up with what played to the sample; punch-in and out; the count-in's clicks before the start
TEST(multitrack_recording_lines_up) {
    fs::path dir = scratch("rec");
    Rig r("(patch (obj b buffer~ imp 100))");
    fillBuffer(r, "b", 4410, [](long i) { return i == 0 ? 1.f : 0.f; });
    std::string err;
    auto d = ScoreDoc::make("rec");
    CHECK(d->saveAs((dir / "rec.score").string(), err));
    tl::addSound(*d, "imp", true, "A", 0.5, 0.1);
    tl::addTrack(*d, "B");
    CHECK(mt::setArm(*d, "B", true, 1, 1));
    Box* player = d->player(&r.p);
    CHECK(player && player->wantsInput());
    const int L = 300;
    Loopback dev(r, L);
    mt::SessionRecorder rec;
    CHECK(rec.start(d, &r.p, 0.0, L, err));
    CHECK(mt::recordingOf(player) != nullptr);
    dev.run(44100);
    std::string msg;
    CHECK(rec.stop(msg) && rec.added.size() == 1);
    CHECK(!mt::recordingOf(player));
    auto take = d->score().find(rec.added[0]);
    CHECK(take && take->row == "B" && take->kind == sc::Kind::Sound);
    CHECK(fs::exists(dir / "rec_media") && fs::exists(dir / "rec_media" / "B take 1.wav"));
    CHECK(take->at == 0 && (long)std::floor(take->from * 44100) == L);
    // played again with A muted: B's take sounds where A's click was, to the sample
    CHECK(mt::setArm(*d, "B", false));
    sc::Strip m;
    m.mute = true;
    tl::setStrip(*d, "A", m);
    auto t = transportOf(player->patch);
    t->locate(player->patch, 0);
    t->play(player->patch);
    auto y = dev.run(44100);
    t->stop(player->patch);
    auto Lc = etest::channel(y, 2, 0);
    long peak = (long)(std::max_element(Lc.begin(), Lc.end()) - Lc.begin());
    CHECK(peak == 22050 && near(Lc[(size_t)peak], kHalf * kHalf, 1e-3f));
    // punch-in and out: only from 0.25 s to 0.75 s is recorded, wherever the transport starts
    tl::setStrip(*d, "A", sc::Strip{});
    CHECK(mt::setArm(*d, "B", true));
    mt::SessionSettings ss = mt::sessionSettings(d->score());
    ss.punch = true;
    ss.punchIn = 0.25;
    ss.punchOut = 0.75;
    CHECK(mt::setSession(*d, ss, "Punch"));
    CHECK(rec.start(d, &r.p, 0.0, L, err));
    dev.run(44100);
    CHECK(rec.stop(msg) && rec.added.size() == 1);
    take = d->score().find(rec.added[0]);
    CHECK(take && (long)std::floor(take->at * 44100) == 11025 - L && std::fabs(take->dur - 0.5) < 1e-9);
    // the count-in: a bar before the playhead (2 s at 120 bpm), clicks there, the take from the playhead
    ss.punch = false;
    ss.countIn = 1;
    CHECK(mt::setSession(*d, ss, "Count-in"));
    CHECK(rec.start(d, &r.p, 2.5, L, err));
    y = dev.run(44100 * 3);
    CHECK(rec.stop(msg) && rec.added.size() == 1);
    take = d->score().find(rec.added[0]);
    CHECK(take && (long)std::floor(take->at * 44100) == 2 * 44100 + 22050 - L);
    Lc = etest::channel(y, 2, 0);
    CHECK(etest::peakOf(Lc, 22050, 22050 + 1000) > 0.1f && etest::peakOf(Lc, 44100, 44100 + 1000) > 0.1f);   // beats 2, 3
    CHECK(d->canUndo() && d->undoName() == "Record");
    // nothing armed: said, nothing started
    CHECK(mt::setArm(*d, "B", false));
    CHECK(!rec.start(d, &r.p, 0, L, err) && err.find("armed") != std::string::npos);
    d.reset();
    r.p.collectGarbage(true);
}

// The render: the master in N channels and each track's output as a file of its own (stems)
TEST(multitrack_render_stems) {
    fs::path dir = scratch("stems");
    SampleBuffer click;
    click.channels = 1;
    click.sr = 44100;
    click.frames = 100;
    click.data.assign(100, 0.f);
    click.data[0] = 1.f;
    std::string err;
    CHECK(writeWavFile((dir / "click.wav").string(), click, err, 24));
    auto d = ScoreDoc::make("st");
    CHECK(d->saveAs((dir / "st.score").string(), err));
    tl::addSound(*d, (dir / "click.wav").string(), false, "A", 0.5, 0.002);
    tl::addSound(*d, (dir / "click.wav").string(), false, "B", 1, 0.002);
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    bool done = false, ok = false;
    std::string out = (dir / "mix.wav").string();
    {
        ScoreRender render;
        CHECK(render.start(d->score(), d->folder(), out, 4, 44100, [&](bool k, const std::string&) { done = true; ok = k; }, err, true));
    }
    jobsSynchronous = sync;
    CHECK(done && ok);
    SampleBuffer mix, a, b;
    CHECK(decodeAudioFile(out, mix, err) && mix.channels == 4);
    CHECK(decodeAudioFile(mt::stemPath(out, "A"), a, err) && a.channels == 4);
    CHECK(decodeAudioFile(mt::stemPath(out, "B"), b, err));
    auto hits = [](const SampleBuffer& s) {
        std::vector<long> h;
        for (long f = 0; f < s.frames; ++f) {
            float m = 0;
            for (int c = 0; c < s.channels; ++c) m = std::max(m, std::fabs(s.at(f, c)));
            if (m > 0.3f) h.push_back(f);
        }
        return h;
    };
    auto hm = hits(mix), ha = hits(a), hb = hits(b);
    CHECK(hm.size() == 2 && hm[0] == 22050 && hm[1] == 44100);
    CHECK(ha.size() == 1 && ha[0] == 22050);
    CHECK(hb.size() == 1 && hb[0] == 44100);
}

// The threaded paths: the audio thread runs (tracks on several cores) while the interface moves faders, arms, adds
// tracks and buses, records and stops, undoes: nothing torn (ThreadSanitizer), nothing allocated there
TEST(multitrack_threaded_session) {
    fs::path dir = scratch("thr");
    writeText(dir / "halve.moritz",
              "(patch (obj i inlet~ 1) (obj j inlet~ 2) (obj a *~ 0.5) (obj b *~ 0.5) (obj o outlet~ 1) (obj p outlet~ 2)"
              " (connect i 0 a 0) (connect j 0 b 0) (connect a 0 o 0) (connect b 0 p 0))");
    Rig r("(patch (obj b buffer~ imp 100) (obj k buffer~ one 100))");
    fillBuffer(r, "b", 4410, [](long i) { return i % 50 == 0 ? 1.f : 0.f; });
    fillBuffer(r, "k", 44100, [](long) { return 0.1f; });
    std::string err;
    auto d = ScoreDoc::make("thr");
    CHECK(d->saveAs((dir / "thr.score").string(), err));
    for (int k = 0; k < 8; ++k) tl::addSound(*d, k % 2 ? "imp" : "one", true, "t" + std::to_string(k), 0.1 * k, 0.5);
    mt::addBus(*d, "fx");
    Box* player = d->player(&r.p);
    auto t = transportOf(player->patch);
    t->setLoop(player->patch, true, 0, 1);
    t->play(player->patch);
    mt::TrackPool::minTracks.store(2);
    std::atomic<bool> stop{false};
    std::atomic<long> blocks{0};
    std::thread audio([&] {
        std::vector<float> in(64, 0.05f), buf(2 * 64);
        while (!stop) {
            r.p.process(in.data(), 1, buf.data(), 2, 64);
            blocks.fetch_add(1);
        }
    });
    mt::SessionRecorder rec;
    std::mt19937 rng(5);
    for (int k = 0; k < 120; ++k) {
        std::string tr = "t" + std::to_string(rng() % 8);
        mt::stripOf(player, tr)->gain.store((float)(rng() % 100) / 100.f);
        if (k % 7 == 0) mt::setSend(*d, tr, "fx", 0.5);
        if (k % 11 == 0) mt::setTrackPatch(*d, tr, "insert", k % 22 ? (dir / "halve.moritz").string() : "");
        if (k % 13 == 0) tl::addSound(*d, "imp", true, tr, (rng() % 100) / 100.0, 0.2);
        if (k % 17 == 0) { d->undo(); d->redo(); }
        if (k == 30) {
            mt::setArm(*d, "t1", true, 1, 1);
            CHECK(rec.start(d, &r.p, 0.2, 64, err));
        }
        if (k == 90) {
            std::string msg;
            rec.stop(msg);
            t->play(player->patch);
        }
        r.p.runDue(r.p.time() + 0.01);
        r.p.collectGarbage();
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    stop = true;
    audio.join();
    mt::TrackPool::minTracks.store(4);
    CHECK(blocks.load() > 0);
    d.reset();
    r.p.collectGarbage(true);
}
