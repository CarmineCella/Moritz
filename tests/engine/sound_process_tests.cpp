// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// sound_process_tests.cpp — the sound editor's second part without its window: markers and regions (moved by
// edits, undone, written and read back, untrusted files refused), process results put in place of the selection
// (channels, rates, undo), the Process menu's engines (each against what it must do), a patch and a Musil script
// run over a selection, the analyses that place markers and regions, the Draw tool's stroke, paste between rates,
// and record~'s writes seen by an editor.

#include "rig.h"
#include "fileio.h"
#include "sound_doc.h"
#include "sound_marks.h"
#include "sound_process.h"
#include "music/score_text.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <thread>

#include <FL/Fl.H>

using namespace moritz;
using etest::Rig;

namespace {
SampleBuffer counting(long frames, int ch, double sr = 44100) {
    SampleBuffer b;
    b.channels = ch;
    b.sr = sr;
    b.frames = frames;
    for (long f = 0; f < frames; ++f)
        for (int c = 0; c < ch; ++c) b.data.push_back((float)f + (float)c / 10.f);
    return b;
}
SampleBuffer sine(long frames, double hz, double sr = 44100, int ch = 1, double amp = 0.5) {
    SampleBuffer b;
    b.channels = ch;
    b.sr = sr;
    b.frames = frames;
    for (long f = 0; f < frames; ++f)
        for (int c = 0; c < ch; ++c) b.data.push_back((float)(amp * std::sin(2 * M_PI * hz * f / sr)));
    return b;
}
std::string temp(const std::string& name) {
    const char* t = std::getenv("TMPDIR");
    std::string d = t && *t ? t : "/tmp";
    if (d.back() != '/') d += '/';
    return d + name;
}
double rms(const std::vector<float>& x, size_t a = 0, size_t b = ~size_t(0)) {
    b = std::min(b, x.size());
    double s = 0;
    for (size_t k = a; k < b; ++k) s += (double)x[k] * x[k];
    return b > a ? std::sqrt(s / (double)(b - a)) : 0;
}
// a process run as the editor runs it: a job (synchronous here, as moritz --render)
bool process(const ProcessSpec& s, const SampleBuffer& in, SampleBuffer& out, std::string& msg) {
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    Job job;
    bool ok = false;
    job.start([&](Job& j) { ok = runProcess(s, in, out, msg, j); }, [] {});
    jobsSynchronous = sync;
    return ok;
}
SoundMarks someMarks() {
    SoundMarks m;
    m.markers = {{100, "a", "hand"}, {500, "b", "onsets"}, {900, "c \"quoted\"", "beats"}};
    Region r;
    r.start = 400;
    r.end = 600;
    r.name = "hit";
    r.type = 2;
    r.energy = 0.25;
    r.pitch = 62.5;
    r.source = "types";
    m.regions = {r};
    return m;
}
}  // namespace

TEST(marks_move_with_edits_and_undo) {
    SoundDoc d;
    d.setSound(counting(1000, 1));
    CHECK(d.setMarks("Add Marker", someMarks()));
    CHECK(d.undoName() == "Add Marker" && d.marks() == someMarks());
    CHECK(!d.setMarks("Again", someMarks()));            // nothing changed: no step
    d.select(450, 550);
    CHECK(d.erase());                                    // a marker inside goes to the edge, the rest moves
    CHECK(d.marks().markers[1].at == 450 && d.marks().markers[2].at == 800 && d.marks().markers[0].at == 100);
    CHECK(d.marks().regions.size() == 1 && d.marks().regions[0].start == 400 && d.marks().regions[0].end == 500);
    CHECK(d.undo());                                     // the samples and the markers, together
    CHECK(d.frames() == 1000 && d.marks() == someMarks());
    CHECK(d.redo() && d.marks().markers[2].at == 800);
    CHECK(d.undo() && d.undo());                         // the marks' own step
    CHECK(d.marks().empty() && !d.canUndo());
    CHECK(d.redo() && d.marks() == someMarks());
    d.select(100, 200);                                  // processed in place: nothing moves
    CHECK(d.reverse() && d.marks() == someMarks());
    d.select(0, 0);
    d.playhead = 0;                                      // inserted before: all move
    d.select(0, 50);
    CHECK(d.copy());
    d.select(0, 0);
    CHECK(d.paste() && d.marks().markers[0].at == 150 && d.marks().regions[0].start == 450);
    d.select(350, 700);                                  // a region inside what is deleted goes
    CHECK(d.erase() && d.marks().regions.empty());
}

TEST(marks_text_round_trip_and_bad_files) {
    SoundMarks m = someMarks(), back;
    std::string err, t = marksToText(m, 44100, 1000 / 44100.0, "x.wav");
    CHECK(marksFromText(t, 44100, 1000, back, err));
    CHECK(back == m);
    CHECK(t.find("(sound-marks (version 1)") != std::string::npos && t.find("(type 2)") != std::string::npos);
    CHECK(marksFromText(t, 22050, 1000, back, err) && back.markers[1].at == 250);   // in seconds: another rate
    CHECK(marksFromText(t, 44100, 450, back, err) && back.markers[2].at == 450 && back.regions[0].end == 450);
    const char* bad[] = {
        "(sound-marks (marker nan \"x\"))", "(sound-marks (marker -1 \"x\"))", "(sound-marks (region 2 1 \"x\"))",
        "(sound-marks (region 0 1 \"x\" (pitch 1e9)))", "(other)", "(sound-marks (marker 1 \"x\")",
        "(sound-marks (marker 1e300))", "(sound-marks (marker (1) \"x\"))", "(sound-marks 3)",
        "(sound-marks (region 0 1 \"x\" (type 0.5x)))"};
    for (const char* b : bad) {
        SoundMarks keep = m;
        err.clear();
        bool ok = marksFromText(b, 44100, 1000, keep, err);
        CHECK(!ok && !err.empty() && keep == m);         // refused, and said; nothing half read
        if (ok) std::printf("  accepted: %s\n", b);
    }
    std::string deep = "(sound-marks ";
    for (int k = 0; k < 100000; ++k) deep += "(";
    CHECK(!marksFromText(deep, 44100, 1000, back, err) && err.find("deep") != std::string::npos);
    std::string big(kMaxMarksFile + 10, ' ');
    CHECK(!marksFromText(big, 44100, 1000, back, err));
    std::string many = "(sound-marks";
    for (size_t k = 0; k <= kMaxMarks; ++k) many += " (marker 0)";
    CHECK(!marksFromText(many + ")", 44100, 1000, back, err) && err.find("too many") != std::string::npos);
    CHECK(marksFromText("(sound-marks (marker 0.001) (future 1 2) (region 0 0.002 (later 3)))", 44100, 1000, back, err));
    CHECK(back.markers.size() == 1 && back.regions.size() == 1 && back.markers[0].source == "hand");   // later forms left out
}

TEST(marks_kept_beside_the_file) {
    std::string path = temp("moritz-engine-marks.wav"), side = marksPath(path), err;
    std::remove(path.c_str());
    std::remove(side.c_str());
    SoundDoc d;
    d.setSound(sine(1000, 440));
    CHECK(d.saveTo(path, err));
    CHECK(!fileio::isFile(side));                        // no markers: no file
    CHECK(d.setMarks("Markers", someMarks()) && d.dirty());
    CHECK(d.save(err) && fileio::isFile(side) && !d.dirty());
    SoundDoc e;
    CHECK(e.load(path, err) && e.marks() == someMarks() && !e.canUndo());
    CHECK(e.setMarks("Clear", SoundMarks()) && e.save(err) && !fileio::isFile(side));   // none left: removed
    { std::ofstream f(side); f << "(sound-marks (marker nan))"; }
    SoundDoc g;
    CHECK(g.load(path, err) && g.marks().empty() && g.marksUnreadable() && g.message.find("can't be read") != std::string::npos);
    g.select(0, 10);
    CHECK(g.silence() && g.save(err));                   // saved: the unreadable file is left as it is
    std::string kept;
    CHECK(fileio::read(side, kept) && kept == "(sound-marks (marker nan))");
    std::remove(path.c_str());
    std::remove(side.c_str());
}

TEST(marks_of_a_buffer_stay_with_it) {
    Rig r("(patch (obj b buffer~ snd 100 1))");
    BufferBox* b = findBuffer(&r.p, "snd");
    b->replace(counting(1000, 1));
    {
        SoundDoc d;
        d.bindBuffer(&r.p, "snd");
        CHECK(d.setMarks("Markers", someMarks()));
        CHECK(b->marks && *b->marks == someMarks());
    }
    SoundDoc e;                                          // the editor opened again: they are there
    e.bindBuffer(&r.p, "snd");
    CHECK(e.marks() == someMarks());
}

TEST(process_result_replaces_the_selection) {
    SoundDoc d;
    d.setSound(counting(100, 2, 44100));
    d.select(10, 20);
    SampleBuffer r;                                      // mono at half the rate: 5 frames -> 10, on both channels
    r.channels = 1;
    r.sr = 22050;
    r.frames = 5;
    r.data = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    CHECK(d.replaceSelection("pvoc", r));
    CHECK(d.frames() == 100 && d.sel.start == 10 && d.sel.end == 20 && d.undoName() == "pvoc");
    float mid = d.sound()->data[(size_t)15 * 2 + 1];
    CHECK(std::fabs(mid - 0.5f) < 0.05f && d.sound()->data[(size_t)25 * 2] == 25.f);
    CHECK(d.undo());
    CHECK(d.sound()->data[(size_t)15 * 2 + 1] == 15.1f);
    d.select(0, 0);                                      // nothing selected: the whole sound
    SampleBuffer two = counting(3, 2);
    CHECK(d.replaceSelection("x", two) && d.frames() == 3);
    CHECK(d.undo() && d.frames() == 100);
}

TEST(paste_resamples_to_the_sound_rate) {
    SoundDoc a, b;
    a.setSound(sine(2205, 441, 22050));                  // 0.1 s at 22050
    b.setSound(sine(4410, 441, 44100));
    a.selectAll();
    CHECK(a.copy());
    b.select(0, 0);
    b.playhead = 4410;
    CHECK(b.paste());
    CHECK(std::labs(b.frames() - 8820) <= 1);            // its duration kept
    CHECK(b.message.find("resampled from 22050 to 44100") != std::string::npos);
    float worst = 0;                                     // and its pitch: the sine goes on
    for (long f = 4500; f < 8700; ++f)
        worst = std::max(worst, std::fabs(b.sound()->data[(size_t)f] - (float)(0.5 * std::sin(2 * M_PI * 441 * (f - 4410) / 44100.0))));
    CHECK(worst < 0.01f);
}

TEST(draw_stroke_is_one_step) {
    SoundDoc d;
    d.setSound(counting(50, 2));
    std::vector<float> f = {1, 2, 3, 4, 5, 6};           // frames 10..12, both channels
    CHECK(d.replaceFrames("Draw", 10, f));
    CHECK(d.sound()->data[20] == 1.f && d.sound()->data[25] == 6.f && d.sound()->data[26] == 13.f && d.undoName() == "Draw");
    CHECK(d.undo() && d.sound()->data[20] == 10.f && !d.canUndo());
    CHECK(!d.replaceFrames("Draw", 49, f));              // past the end: nothing
}

TEST(process_engines_do_what_they_say) {
    std::string msg;
    SampleBuffer low = sine(22050, 200), high = sine(22050, 8000), out;
    ProcessSpec f;
    f.kind = "filter";
    f.p = {{"type", "lowpass"}, {"freq", "1000"}};
    CHECK(process(f, low, out, msg) && out.frames == low.frames);
    CHECK(rms(out.data, 4000) > 0.33);                   // 200 Hz passes (0.354 is the sine's RMS)
    CHECK(process(f, high, out, msg) && rms(out.data, 4000) < 0.01);   // 8 kHz does not

    SampleBuffer mix = sine(44100, 300, 44100, 2);
    for (long k = 0; k < mix.frames; k += 4410) mix.data[(size_t)k * 2] += 0.8f;   // clicks on the left
    ProcessSpec h;
    h.kind = "hpss";
    SampleBuffer harm, perc;
    CHECK(process(h, mix, harm, msg) && harm.channels == 2 && harm.frames == mix.frames);
    h.p["keep"] = "percussive";
    CHECK(process(h, mix, perc, msg));
    float worst = 0;                                     // the two parts add up to the sound
    for (size_t k = 0; k < mix.data.size(); ++k) worst = std::max(worst, std::fabs(harm.data[k] + perc.data[k] - mix.data[k]));
    CHECK(worst < 1e-3f);

    ProcessSpec n;
    n.kind = "nmf";
    n.p = {{"components", "2"}, {"keep", "1 2"}, {"iterations", "20"}};
    SampleBuffer both;
    SampleBuffer one = sine(22050, 300);
    CHECK(process(n, one, both, msg) && both.frames == one.frames);
    worst = 0;                                           // every part kept: the sound again
    for (size_t k = 2048; k + 2048 < one.data.size(); ++k) worst = std::max(worst, std::fabs(both.data[k] - one.data[k]));
    CHECK(worst < 1e-3f);
    n.p["keep"] = "7";
    CHECK(!process(n, one, both, msg) && msg.find("keep which") != std::string::npos);

    ProcessSpec v;
    v.kind = "pvoc";
    v.p = {{"stretch", "2"}};
    CHECK(process(v, one, out, msg) && std::labs(out.frames - 2 * one.frames) < 2048);
    v.p = {{"transpose", "12"}};
    CHECK(process(v, one, out, msg) && std::labs(out.frames - one.frames) < 2048);
    auto crossings = [](const std::vector<float>& x) {
        int c = 0;
        for (size_t k = 4096; k + 4096 < x.size(); ++k) c += (x[k - 1] < 0) != (x[k] < 0);
        return c;
    };
    double ratio = (double)crossings(out.data) / crossings(one.data);
    CHECK(ratio > 1.9 && ratio < 2.1);                   // an octave up

    ProcessSpec c;
    c.kind = "condec";
    SampleBuffer imp;
    imp.channels = 1;
    imp.frames = 3;
    imp.data = {0.f, 1.f, 0.f};                          // a delay of one sample
    c.other = std::make_shared<const SampleBuffer>(imp);
    CHECK(process(c, low, out, msg) && out.frames == low.frames + 2);
    worst = 0;
    for (long k = 0; k < low.frames; ++k) worst = std::max(worst, std::fabs(out.data[(size_t)k + 1] - low.data[(size_t)k]));
    CHECK(worst < 1e-5f);
    c.other.reset();
    CHECK(!process(c, low, out, msg) && msg.find("choose") != std::string::npos);

    ProcessSpec x;
    x.kind = "cross";
    x.other = std::make_shared<const SampleBuffer>(sine(8000, 600));
    CHECK(process(x, one, out, msg) && out.frames == one.frames && rms(out.data) > 0.01);

    ProcessSpec p;
    p.kind = "pursuit";
    p.p = {{"atoms", "40"}};
    SampleBuffer s = sine(8192, 440, 44100, 1, 0.3);
    CHECK(process(p, s, out, msg) && out.frames == s.frames);
    double err = 0, sig = 0;
    for (long k = 1024; k < s.frames - 1024; ++k) {
        err += std::pow(out.data[(size_t)k] - s.data[(size_t)k], 2);
        sig += std::pow(s.data[(size_t)k], 2);
    }
    CHECK(10 * std::log10(sig / std::max(1e-12, err)) > 10);   // rebuilt from atoms, close to it
    p.p["dictionary"] = "nonesuch";
    CHECK(!process(p, s, out, msg) && msg.find("no atoms") != std::string::npos);

    ProcessSpec t;
    t.kind = "typematch";
    SampleBuffer a = sine(44100, 220), b = sine(44100, 880);
    for (long k = 0; k < b.frames; ++k) b.data[(size_t)k] *= (float)(0.5 + 0.5 * std::sin(k * 0.0005));
    t.other = std::make_shared<const SampleBuffer>(b);
    CHECK(process(t, a, out, msg) && out.frames > 0 && rms(out.data) > 0.01);
    ProcessSpec u;
    u.kind = "nonesuch";
    CHECK(!process(u, a, out, msg) && msg.find("unknown") != std::string::npos);
}

TEST(process_stops_when_cancelled) {
    bool sync = jobsSynchronous;
    jobsSynchronous = false;
    Job job;
    std::atomic<bool> result{true};
    ProcessSpec p;
    p.kind = "pvoc";
    p.p = {{"stretch", "4"}};
    auto in = std::make_shared<SampleBuffer>(sine(44100 * 20, 300));
    job.start([in, p, &result](Job& j) {
        SampleBuffer out;
        std::string m;
        result = runProcess(p, *in, out, m, j);
    }, [] {});
    job.cancel();                                        // waits for it
    CHECK(!result.load());
    jobsSynchronous = sync;
}

TEST(process_through_a_patch) {
    std::string path = temp("moritz-engine-half.moritz");
    {
        std::ofstream f(path);
        f << "(patch\n  (obj i inlet~ (pos 20 20))\n  (obj g *~ 0.5 (pos 20 60))\n  (obj o outlet~ (pos 20 100))\n"
             "  (connect i 0 g 0)\n  (connect g 0 o 0))\n";
    }
    SampleBuffer in = counting(1000, 2);
    for (auto& x : in.data) x /= 1000.f;
    PatchProcess pp;
    std::string err;
    CHECK(pp.prepare(path, in, 0, err));
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    Job job;
    job.start([&](Job& j) { pp.run(j); }, [] {});
    jobsSynchronous = sync;
    std::vector<std::string> prints;
    SampleBuffer out = pp.take(prints);
    CHECK(out.frames == 1000 && out.channels == 1);
    float worst = 0;                                     // its one inlet~ takes the left channel, halved
    for (long k = 0; k < 1000; ++k) worst = std::max(worst, std::fabs(out.data[(size_t)k] - 0.5f * in.data[(size_t)k * 2]));
    CHECK(worst < 1e-6f);
    {
        std::ofstream f(path);
        f << "(patch (obj g *~ 0.5 (pos 20 60)))\n";
    }
    PatchProcess none;
    CHECK(!none.prepare(path, in, 0, err) && err.find("inlet~") != std::string::npos);
    { std::ofstream f(path); f << "(patch (obj"; }
    PatchProcess broken;
    CHECK(!broken.prepare(path, in, 0, err));
    PatchProcess missing;
    CHECK(!missing.prepare(temp("no-such-moritz-patch.moritz"), in, 0, err) && err.find("can't read") != std::string::npos);
    std::remove(path.c_str());
}

TEST(process_with_a_musil_script) {
    std::string path = temp("moritz-engine-process.mu");
    {
        std::ofstream f(path);
        f << "; halves every channel\nfunction process (x sr) (map x (function (c) (* c 0.5)))\n";
    }
    SampleBuffer in = counting(100, 2);
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    bool ok = false;
    SampleBuffer out;
    std::string err;
    ScriptProcess sp;
    sp.start(path, in, [&](bool k, SampleBuffer b, std::string e) { ok = k; out = std::move(b); err = e; });
    CHECK(ok && out.channels == 2 && out.frames == 100 && out.data[3] == 0.55f && out.sr == in.sr);
    {
        std::ofstream f(path);
        f << "function process (x sr)\n  (vec 1 2 3)\n";                // a vector: one channel
    }
    sp.start(path, in, [&](bool k, SampleBuffer b, std::string e) { ok = k; out = std::move(b); err = e; });
    CHECK(ok && out.channels == 1 && out.frames == 3);
    {
        std::ofstream f(path);
        f << "; nothing\n\nfunction process (x sr)\n  (undefined-thing x)\n";
    }
    sp.start(path, in, [&](bool k, SampleBuffer b, std::string e) { ok = k; out = std::move(b); err = e; });
    CHECK(!ok && err.find("undefined-thing") != std::string::npos && err.find(":4") != std::string::npos);
    {
        std::ofstream f(path);
        f << "var y 1\n";
    }
    sp.start(path, in, [&](bool k, SampleBuffer b, std::string e) { ok = k; out = std::move(b); err = e; });
    CHECK(!ok && err.find("defines no function process") != std::string::npos);
    {
        std::ofstream f(path);
        f << "function process (x sr) \"a string\"\n";
    }
    sp.start(path, in, [&](bool k, SampleBuffer b, std::string e) { ok = k; out = std::move(b); err = e; });
    CHECK(!ok && err.find("no sound") != std::string::npos);
    {
        std::ofstream f(path);
        f << "function process (x sr) (system \"touch /tmp/moritz-should-not\")\n";
    }
    sp.start(path, in, [&](bool k, SampleBuffer b, std::string e) { ok = k; out = std::move(b); err = e; });
    CHECK(!ok);                                          // a code box's permissions: no programs
    jobsSynchronous = sync;
    std::remove(path.c_str());
}

TEST(process_script_stopped_from_another_thread) {
    std::string path = temp("moritz-engine-forever.mu");
    {
        std::ofstream f(path);
        f << "function process (x sr) { var k 0\n  while true { set k (+ k 1) }\n}\n";
    }
    bool sync = jobsSynchronous;
    jobsSynchronous = false;
    std::atomic<int> called{0};
    {
        ScriptProcess sp;
        sp.start(path, counting(10, 1), [&](bool, SampleBuffer, std::string) { ++called; });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(sp.running());                             // it loops
        sp.stop();                                       // it stops at its next step; done is never called
    }                                                    // and the process is gone while its thread ends
    for (int k = 0; k < 200; ++k) {
        Fl::wait(0.01);
        drainMain();
    }
    CHECK(called.load() == 0);
    jobsSynchronous = sync;
    std::remove(path.c_str());
}

TEST(markers_from_onsets_beats_and_regions) {
    SampleBuffer s;
    s.channels = 1;
    s.sr = 44100;
    s.frames = 44100 * 4;
    s.data.assign((size_t)s.frames, 0.f);
    std::vector<long> hits;
    for (int k = 0; k < 8; ++k) hits.push_back(22050 * k + 5000);
    for (long h : hits)                                  // decaying bursts of a 440 Hz tone
        for (long f = 0; f < 15000; ++f) s.data[(size_t)(h + f)] += (float)(0.6 * std::exp(-f / 3000.0) * std::sin(2 * M_PI * 440 * f / 44100.0));
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    Job job;
    std::vector<long> on, beats;
    std::vector<Region> regs, types;
    double bpm = 0;
    job.start([&](Job& j) {
        on = onsetFrames(s, MarkOptions(), j);
        beats = beatFrames(s, j, &bpm);
        regs = onsetRegions(s, MarkOptions(), j);
        MarkOptions o;
        o.types = 3;
        types = typeRegions(s, o, j);
    }, [] {});
    jobsSynchronous = sync;
    CHECK(on.size() == hits.size());
    for (size_t k = 0; k < on.size() && k < hits.size(); ++k) CHECK(std::labs(on[k] - hits[k]) < 512);
    CHECK(bpm > 100 && bpm < 140 && beats.size() >= 4);   // a burst every half second: 120 a minute
    CHECK(regs.size() == hits.size() + 1);               // before the first, then between each
    if (regs.size() > 2) {
        CHECK(regs[1].start == on[0] && regs[1].end == on[1] && regs[1].name == "event 2");
        CHECK(regs[1].energy > 0.05 && std::fabs(regs[1].pitch - 69) < 0.5);    // A4, measured
        CHECK(regs[0].pitch == 0);                       // silence: no pitch
    }
    CHECK(!types.empty() && types[0].type >= 0 && types[0].source == "types");
    CHECK(pitchName(69) == "A4" && pitchName(61.2) == "C#4" && pitchName(0) != "C-1");
}

TEST(sound_editor_sees_record_writes) {
    Rig r("(patch (obj b buffer~ snd 100 1) (obj o cycle~ 441) (obj rec record~ snd) (connect o 0 rec 0))");
    BufferBox* b = findBuffer(&r.p, "snd");
    SoundDoc d;
    d.bindBuffer(&r.p, "snd");
    CHECK(d.frames() > 0 && d.sound()->data[100] == 0.f);
    CHECK(!d.followBuffer());
    r.box("rec")->message(0, {Atom(1.f)});
    r.run(512);
    CHECK(b->writes.load() > 0);
    CHECK(!d.followBuffer(false));                       // asked about replacements only: not yet
    CHECK(d.followBuffer());                             // the writes: read again
    CHECK(d.sound()->data[100] != 0.f && d.message.find("record~") != std::string::npos);
    CHECK(!d.followBuffer());
}


TEST(regions_to_score_read_back_as_sound_events) {
    SoundMarks m = someMarks();
    Region r;
    r.start = 44100;
    r.end = 66150;
    r.name = "second";
    m.regions.push_back(r);
    std::string t = scoreText(m, 44100, "voice", true, "voice regions");
    auto s = musil::score::readText(t, "regions.score");      // as the score object reads it
    CHECK(s && s->size() == 2 && s->name == "voice regions");
    if (s && s->size() == 2) {
        const auto& e = *s->events()[0];
        CHECK(e.kind == musil::score::Kind::Sound && e.buffer && e.source == "voice" && e.label == "hit");
        CHECK(std::fabs(e.at - 400 / 44100.0) < 1e-9 && std::fabs(e.dur - 200 / 44100.0) < 1e-9);
        CHECK(std::fabs(e.from - e.at) < 1e-9 && std::fabs(e.to - 600 / 44100.0) < 1e-9);
        const auto& f = *s->events()[1];
        CHECK(f.at == 1.0 && f.dur == 0.5 && f.label == "second" && f.row == "regions");
    }
}
