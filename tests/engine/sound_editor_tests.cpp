// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// sound_editor_tests.cpp — the sound editor without its window: the edits and their undo (exact samples, several
// channels, the edges), a buffer~ edited in place, saving, the waveform's summary, the spectrogram, and the preview
// voice the audio thread plays (the right samples, stopped, the sound kept alive while it plays, from two threads).

#include "rig.h"
#include "preview.h"
#include "sound_doc.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <thread>

using namespace moritz;
using etest::Rig;

namespace {
// frames of ch channels, sample (f, c) = f + c / 10 (each one tells where it came from)
SampleBuffer counting(long frames, int ch, double sr = 44100) {
    SampleBuffer b;
    b.channels = ch;
    b.sr = sr;
    b.frames = frames;
    for (long f = 0; f < frames; ++f)
        for (int c = 0; c < ch; ++c) b.data.push_back((float)f + (float)c / 10.f);
    return b;
}
float at(const SoundDoc& d, long f, int c) { return d.sound()->data[(size_t)f * d.channels() + c]; }
bool same(const SampleBuffer& a, const SampleBuffer& b) {
    return a.channels == b.channels && a.frames == b.frames && a.data == b.data;
}
std::string temp(const std::string& name) {
    const char* t = std::getenv("TMPDIR");
    std::string d = t && *t ? t : "/tmp";
    if (d.back() != '/') d += '/';
    return d + name;
}
}  // namespace

TEST(sound_cut_undo_redo_exact) {
    SoundDoc d;
    d.setSound(counting(10, 2));
    SampleBuffer before = *d.sound();
    d.select(5, 2);                                      // in any order
    CHECK(d.sel.start == 2 && d.sel.end == 5);
    CHECK(d.cut());
    CHECK(d.frames() == 7);
    CHECK(at(d, 1, 1) == 1.1f && at(d, 2, 0) == 5.f && at(d, 2, 1) == 5.1f);
    CHECK(d.sel.empty() && d.playhead == 2);
    CHECK(soundClipboard() && soundClipboard()->frames == 3 && soundClipboard()->data[0] == 2.f);
    CHECK(d.undoName() == "Cut");
    CHECK(d.undo());
    CHECK(same(*d.sound(), before));
    CHECK(d.sel.start == 2 && d.sel.end == 5);
    CHECK(d.redo());
    CHECK(d.frames() == 7 && at(d, 2, 0) == 5.f);
    CHECK(!d.canRedo() && d.canUndo());
}

TEST(sound_paste_inserts_and_replaces) {
    SoundDoc d;
    d.setSound(counting(6, 1));
    d.select(0, 2);
    CHECK(d.copy());
    CHECK(!d.canUndo());                                 // a copy is not an edit
    d.select(3, 3);                                      // nothing selected: at the playhead
    d.playhead = 4;
    CHECK(d.paste());
    CHECK(d.frames() == 8);
    float want[] = {0, 1, 2, 3, 0, 1, 4, 5};
    for (int f = 0; f < 8; ++f) CHECK(at(d, f, 0) == want[f]);
    CHECK(d.sel.start == 4 && d.sel.end == 6);           // what was pasted is selected
    d.select(0, 3);                                      // over a selection: it is replaced
    CHECK(d.paste());
    float want2[] = {0, 1, 3, 0, 1, 4, 5};
    CHECK(d.frames() == 7);
    for (int f = 0; f < 7; ++f) CHECK(at(d, f, 0) == want2[f]);
    CHECK(d.undo() && d.undo());
    CHECK(same(*d.sound(), counting(6, 1)));
}

TEST(sound_paste_other_channel_counts) {
    SoundDoc mono, stereo;
    mono.setSound(counting(4, 1));
    stereo.setSound(counting(4, 2));
    mono.select(1, 3);
    CHECK(mono.copy());                                  // mono into stereo: on both channels
    stereo.playhead = 0;
    CHECK(stereo.paste());
    CHECK(stereo.frames() == 6);
    CHECK(at(stereo, 0, 0) == 1.f && at(stereo, 0, 1) == 1.f && at(stereo, 1, 1) == 2.f && at(stereo, 2, 1) == 0.1f);
    stereo.select(4, 6);                                 // frames 2 and 3 of the original: (2, 2.1) (3, 3.1)
    CHECK(stereo.copy());
    mono.select(0, 0);
    mono.playhead = 4;
    CHECK(mono.paste());                                 // stereo into mono: their mean
    CHECK(mono.frames() == 6);
    CHECK(std::fabs(at(mono, 4, 0) - 2.05f) < 1e-6f && std::fabs(at(mono, 5, 0) - 3.05f) < 1e-6f);
    SoundDoc four;                                       // 2 -> 4 channels: channel c takes c % 2
    four.setSound(counting(1, 4));
    CHECK(four.paste());
    CHECK(at(four, 0, 0) == 2.f && at(four, 0, 1) == 2.1f && at(four, 0, 2) == 2.f && at(four, 0, 3) == 2.1f);
}

TEST(sound_edits_need_a_selection) {
    SoundDoc d;
    d.setSound(counting(8, 2));
    d.select(3, 3);
    CHECK(!d.erase() && !d.cut() && !d.trim() && !d.silence() && !d.fadeIn() && !d.fadeOut() && !d.gain(6) &&
          !d.normalize() && !d.reverse());
    CHECK(!d.canUndo());
    CHECK(d.message.find("select") != std::string::npos);
    CHECK(same(*d.sound(), counting(8, 2)));
    CHECK(!d.undo() && !d.redo());
}

TEST(sound_edits_at_the_ends) {
    SoundDoc d;
    d.setSound(counting(8, 1));
    d.select(0, 2);
    CHECK(d.erase());
    CHECK(d.frames() == 6 && at(d, 0, 0) == 2.f);
    d.select(4, 6);
    CHECK(d.erase());
    CHECK(d.frames() == 4 && at(d, 3, 0) == 5.f);
    d.selectAll();
    CHECK(!d.trim());                                    // the whole sound: nothing to trim
    d.select(0, 1);
    CHECK(d.trim());
    CHECK(d.frames() == 1 && at(d, 0, 0) == 2.f);
    d.select(1, 1);
    d.select(-5, 100);                                   // clamped
    CHECK(d.sel.start == 0 && d.sel.end == 1);
    CHECK(d.undo() && d.undo() && d.undo());
    CHECK(same(*d.sound(), counting(8, 1)));
    d.select(2, 6);                                      // trim in the middle, undone and redone
    CHECK(d.trim());
    CHECK(d.frames() == 4 && at(d, 0, 0) == 2.f && at(d, 3, 0) == 5.f);
    CHECK(d.undo());
    CHECK(same(*d.sound(), counting(8, 1)));
    CHECK(d.redo());
    CHECK(d.frames() == 4 && at(d, 0, 0) == 2.f);
}

TEST(sound_zero_length) {
    SoundDoc d;
    SampleBuffer empty;
    empty.channels = 2;
    d.setSound(std::move(empty));
    CHECK(d.frames() == 0 && d.channels() == 2);
    d.selectAll();
    CHECK(d.sel.empty());
    CHECK(!d.cut() && !d.copy() && !d.reverse());
    SoundDoc src;
    src.setSound(counting(3, 2));
    src.selectAll();
    CHECK(src.copy());
    CHECK(d.paste());
    CHECK(d.frames() == 3 && at(d, 2, 1) == 2.1f);
    CHECK(d.undo());
    CHECK(d.frames() == 0);
    soundClipboard() = nullptr;
    CHECK(!d.paste());
}

TEST(sound_process_edits_exact) {
    SoundDoc d;
    SampleBuffer b;
    b.channels = 2;
    b.frames = 4;
    b.data = {1, -1, 1, -1, 1, -1, 1, -1};
    d.setSound(std::move(b));
    SampleBuffer orig = *d.sound();
    d.selectAll();
    CHECK(d.fadeIn());                                   // k / n, as Musil's fade-in
    float in[] = {0.f, .25f, .5f, .75f};
    for (int f = 0; f < 4; ++f) CHECK(at(d, f, 0) == in[f] && at(d, f, 1) == -in[f]);
    SampleBuffer faded = *d.sound();
    CHECK(d.undo());
    CHECK(same(*d.sound(), orig));
    CHECK(d.redo());                                     // made again from what it removed
    CHECK(same(*d.sound(), faded));
    CHECK(d.undo());
    CHECK(d.fadeOut());                                  // (n - 1 - k) / n: the last frame silent
    float out[] = {.75f, .5f, .25f, 0.f};
    for (int f = 0; f < 4; ++f) CHECK(at(d, f, 0) == out[f]);
    CHECK(d.undo());
    d.select(1, 3);
    CHECK(d.gain(20.0 * std::log10(2.0)));
    CHECK(at(d, 0, 0) == 1.f && std::fabs(at(d, 1, 0) - 2.f) < 1e-5f && std::fabs(at(d, 2, 1) + 2.f) < 1e-5f &&
          at(d, 3, 0) == 1.f);
    CHECK(d.normalize(-6.0206));                         // the peak (2) to half
    CHECK(std::fabs(at(d, 1, 0) - .5f) < 1e-4f && at(d, 0, 0) == 1.f);
    CHECK(d.silence());
    CHECK(at(d, 1, 0) == 0.f && at(d, 2, 1) == 0.f && at(d, 3, 1) == -1.f);
    CHECK(!d.normalize());                               // silence stays silent
    CHECK(d.undo() && d.undo() && d.undo());
    CHECK(same(*d.sound(), orig));
    d.setSound(counting(5, 2));
    d.select(1, 4);
    CHECK(d.reverse());
    CHECK(at(d, 0, 0) == 0.f && at(d, 1, 0) == 3.f && at(d, 1, 1) == 3.1f && at(d, 3, 1) == 1.1f && at(d, 4, 0) == 4.f);
    CHECK(d.undo() && same(*d.sound(), counting(5, 2)));
    CHECK(d.redo() && at(d, 1, 0) == 3.f);
}

// undo keeps what an edit changed, not copies of the sound; and no more than its budget
TEST(sound_undo_memory_is_bounded) {
    SoundDoc d;
    d.setSound(counting(100000, 2));
    d.select(1000, 2000);
    CHECK(d.gain(-6));
    CHECK(d.historyBytes() == 1000 * 2 * sizeof(float));   // the frames it changed, once
    d.select(0, 50000);
    CHECK(d.cut());
    CHECK(d.historyBytes() == (1000 + 50000) * 2 * sizeof(float));
    size_t saved = SoundDoc::undoBudget;
    SoundDoc::undoBudget = 3000 * 2 * sizeof(float);
    SoundDoc e;
    e.setSound(counting(100000, 2));
    for (int k = 0; k < 5; ++k) {
        e.select(k * 1000, k * 1000 + 1000);
        CHECK(e.reverse());
    }
    CHECK(e.historyBytes() <= SoundDoc::undoBudget);
    int steps = 0;
    while (e.undo()) ++steps;
    CHECK(steps == 3);
    e.selectAll();
    CHECK(e.reverse());                                  // larger than the budget: the last step is kept anyway
    CHECK(e.canUndo() && e.undo());
    SoundDoc::undoBudget = saved;
}

TEST(sound_editor_changes_the_buffer_in_place) {
    Rig r("(patch (obj b buffer~ snd 1 2) (obj p play~ snd 2) (obj g groove~ snd) (obj o dac~ 1 2)"
          " (connect p 0 o 0) (connect p 1 o 1))");
    BufferBox* b = findBuffer(&r.p, "snd");
    b->replace(counting(100, 2));
    SoundDoc d;
    d.bindBuffer(&r.p, "snd");
    CHECK(d.frames() == 100 && at(d, 99, 1) == 99.1f);
    int patches = 0;
    auto users = d.users(&patches);
    CHECK(users.size() == 2 && users[0] == "play~" && users[1] == "groove~" && patches == 1);
    int v = b->version;
    d.select(10, 20);
    CHECK(d.erase());
    CHECK(b->buf.frames == 90 && b->buf.data[20] == 20.f && b->version == v + 1);
    CHECK(!d.needsSaving());                             // patch memory: nothing to save
    d.select(0, 90);                                     // play~ plays what the editor made
    CHECK(d.gain(-60));
    r.box("p")->message(0, {Atom("start")});
    auto y = r.run(64);
    CHECK(std::fabs(y[2 * 10] - 0.02f) < 1e-6f && std::fabs(y[2 * 10 + 1] - 0.0201f) < 1e-6f);
    CHECK(d.undo() && d.undo());
    CHECK(b->buf.frames == 100 && b->buf.data[2 * 15 + 1] == 15.1f);
    CHECK(!d.followBuffer());                            // its own change
    b->message(0, {Atom("clear")});                      // changed elsewhere: shown again, history cleared
    CHECK(d.followBuffer());
    CHECK(d.frames() == 100 && at(d, 50, 0) == 0.f && !d.canUndo() && !d.canRedo());
    d.select(0, 10);
    b->message(0, {Atom("size"), Atom(1000.f * 50 / 44100.f)});
    CHECK(!d.silence());                                 // the edit sees the buffer~ changed: shown, not edited
    CHECK(d.frames() == b->buf.frames);
    r.p.removeBoxes({b});                                // the buffer~ gone: the editor keeps its samples
    CHECK(!d.buffer() && !d.followBuffer());
    d.select(0, 5);
    CHECK(d.reverse());
}

TEST(sound_save_and_export) {
    SoundDoc d;
    SampleBuffer b;
    b.channels = 2;
    b.sr = 48000;
    b.frames = 300;
    for (long f = 0; f < 300; ++f) {
        b.data.push_back((float)std::sin(f * 0.05) * 0.5f);
        b.data.push_back((float)std::cos(f * 0.05) * 0.25f);
    }
    d.setSound(std::move(b));
    std::string path = temp("moritz-engine-sound-save.wav");
    std::remove(path.c_str());
    std::string err;
    CHECK(!d.save(err));                                 // no file yet
    CHECK(d.saveTo(path, err));
    CHECK(d.savesInPlace() && !d.dirty());
    d.select(0, 100);
    CHECK(d.erase());
    CHECK(d.dirty() && d.needsSaving());
    CHECK(d.undo() && !d.dirty());                     // undone back to what the file holds
    CHECK(d.redo() && d.dirty());
    CHECK(d.save(err));
    SampleBuffer back;
    CHECK(decodeAudioFile(path, back, err));
    CHECK(back.frames == 200 && back.channels == 2 && std::lround(back.sr) == 48000);
    float worst = 0;
    for (size_t k = 0; k < back.data.size(); ++k) worst = std::max(worst, std::fabs(back.data[k] - d.sound()->data[k]));
    CHECK(worst < 1e-6f);                                // 24 bits
    SoundDoc e;
    CHECK(e.load(path, err) && e.frames() == 200 && e.path().find("moritz-engine-sound-save.wav") != std::string::npos);
    CHECK(!d.saveTo(temp("no-such-folder-moritz/x.wav"), err));   // a failed save says so
    CHECK(d.path() == e.path());                                   // and keeps its file
    std::remove(path.c_str());
}

TEST(sound_summary_matches_the_samples) {
    SampleBuffer s;
    s.channels = 2;
    s.sr = 44100;
    s.frames = 200000;
    uint32_t r = 7;
    for (long f = 0; f < s.frames * 2; ++f) {
        r = r * 1664525u + 1013904223u;
        s.data.push_back((float)((double)r / 4294967296.0 * 2 - 1) * (f / 2 % 5000 == 0 ? 1.f : 0.3f));
    }
    WaveSummary w;
    w.build(s);
    for (long a : {0L, 777L, 5000L, 123456L}) {
        for (long n : {10L, 500L, 4096L, 70000L}) {
            long b = std::min(a + n, s.frames);
            float lo, hi, tlo = 1e9f, thi = -1e9f;
            for (long f = a; f < b; ++f) { tlo = std::min(tlo, s.data[(size_t)f * 2 + 1]); thi = std::max(thi, s.data[(size_t)f * 2 + 1]); }
            w.range(s, 1, a, b, lo, hi);
            CHECK(lo <= tlo && hi >= thi);               // never less than the samples
            CHECK(hi - lo <= 2.0f);
        }
    }
    float lo, hi;
    w.range(s, 0, 5000, 5001, lo, hi);                   // one frame: exactly it
    CHECK(lo == s.data[10000] && hi == s.data[10000]);
}

TEST(sound_spectrogram_finds_a_sine) {
    SampleBuffer s;
    s.channels = 1;
    s.sr = 44100;
    s.frames = 44100;
    for (long f = 0; f < s.frames; ++f) s.data.push_back((float)std::sin(2 * M_PI * 1000.0 * f / 44100.0));
    Spectrogram g;
    CHECK(computeSpectrogram(s, g));
    CHECK(g.cols > 10 && g.rows == 256);
    int col = g.cols / 2, best = 0;
    for (int r = 1; r < g.rows; ++r)
        if (g.v[(size_t)col * g.rows + r] > g.v[(size_t)col * g.rows + best]) best = r;
    double f0 = g.rowFreq(best), f1 = g.rowFreq(best + 1);
    CHECK(f0 < 1060 && f1 > 940);
    CHECK(g.v[(size_t)col * g.rows + best] > 240);       // near 0 dB: a full-scale sine
    std::atomic<int> calls{0};
    Spectrogram h;
    CHECK(!computeSpectrogram(s, h, [&](double) { return ++calls < 2; }));   // stopped
}

// ---- the preview voice

TEST(preview_plays_the_right_samples) {
    PreviewPlayer pp;
    auto s = std::make_shared<SampleBuffer>(counting(1000, 1));
    for (auto& v : const_cast<std::vector<float>&>(s->data)) v /= 1000.f;
    uint64_t id = pp.play(s, 100, 300, false);
    CHECK(pp.active(id));
    std::vector<float> out(256 * 2, 0.f);
    long before = etest::audioAllocations();
    CHECK(pp.render(out.data(), 2, 256, 44100));
    if (etest::allocGuardActive()) CHECK(etest::audioAllocations() == before);
    bool ok = true;
    for (int f = 0; f < 256; ++f) {
        float want = f < 200 ? (100.f + f) / 1000.f : 0.f;
        ok = ok && out[(size_t)f * 2] == want && out[(size_t)f * 2 + 1] == want;   // mono on both channels
    }
    CHECK(ok);
    CHECK(!pp.active(id) && pp.position(id) < 0);       // at its end it is done, and handed back
    CHECK(pp.waiting() == 1);
    pp.collect();
    CHECK(pp.waiting() == 0);
    std::weak_ptr<const SampleBuffer> alive = s;
    id = pp.play(s, 0, 0, true);                         // looping, from the start to the end
    s.reset();
    std::fill(out.begin(), out.end(), 0.f);
    for (int k = 0; k < 5; ++k) pp.render(out.data(), 2, 256, 44100);   // 1280 frames: round once and more
    CHECK(pp.active(id) && std::fabs(pp.position(id) - 280.0) < 1e-9);
    CHECK(!alive.expired());                             // alive while it plays
    pp.stop();
    CHECK(!pp.active(id));
    std::fill(out.begin(), out.end(), 0.f);
    pp.render(out.data(), 2, 256, 44100);                // fades out over 5 ms (220 frames), then silent
    CHECK(out[0] != 0.f && out[2 * 250] == 0.f);
    pp.collect();
    CHECK(alive.expired());                              // freed on this thread once the audio let go
}

TEST(preview_at_another_rate_and_replaced) {
    PreviewPlayer pp;
    auto s = std::make_shared<SampleBuffer>(counting(100, 2, 22050));
    uint64_t a = pp.play(s, 0, 0, false);
    std::vector<float> out(16 * 2, 0.f);
    pp.render(out.data(), 2, 16, 44100);                 // half speed: a sample every two frames, interpolated
    CHECK(out[2 * 3] == 1.5f && std::fabs(out[2 * 3 + 1] - 1.6f) < 1e-6f);
    uint64_t b = pp.play(s, 50, 60, false);              // the one before fades out under the new one
    std::fill(out.begin(), out.end(), 0.f);
    pp.render(out.data(), 2, 16, 44100);
    CHECK(!pp.active(a) && pp.active(b));
    CHECK(out[0] > 50.f);                                // both sound at the start of the block
    PreviewPlayer mono;                                  // to one channel: the channels' mean
    mono.play(s, 0, 0, false);
    std::vector<float> one(4, 0.f);
    mono.render(one.data(), 1, 4, 22050);
    CHECK(std::fabs(one[2] - 2.05f) < 1e-6f);
}

// the audio thread plays while the GUI thread plays, edits, closes the document and stops: the samples it reads
// stay alive (a TSan path), and what it hands back is freed on the GUI thread
TEST(preview_threaded_survives_edits_and_closing) {
    PreviewPlayer pp;
    std::atomic<bool> run{true};
    std::atomic<long> blocks{0};
    std::atomic<float> sum{0.f};
    std::thread audio([&] {
        std::vector<float> out(64 * 2);
        while (run.load()) {
            std::fill(out.begin(), out.end(), 0.f);
            pp.render(out.data(), 2, 64, 44100);
            float s = 0;
            for (float v : out) s += std::fabs(v);
            sum.store(sum.load() + s);
            blocks.fetch_add(1);
            std::this_thread::yield();
        }
    });
    auto waitBlocks = [&](long n) {
        long b0 = blocks.load();
        for (int k = 0; k < 2000 && blocks.load() < b0 + n; ++k) std::this_thread::sleep_for(std::chrono::microseconds(200));
    };
    std::weak_ptr<const SampleBuffer> first, last;
    {
        auto doc = std::make_unique<SoundDoc>();
        SampleBuffer b;
        b.channels = 2;
        b.frames = 20000;
        b.data.assign(40000, 0.5f);
        doc->setSound(std::move(b));
        first = doc->sound();
        uint64_t id = pp.play(doc->sound(), 0, 0, true);
        waitBlocks(10);
        CHECK(pp.active(id));
        for (int k = 0; k < 20; ++k) {                   // edits make new snapshots; the old one plays on
            doc->select(k * 100, k * 100 + 3000);
            doc->reverse();
            doc->gain(-1);
            pp.collect();
        }
        waitBlocks(5);
        {                                                // a buffer~ replaced elsewhere while its editor plays it
            Rig r("(patch (obj b buffer~ snd 100 2))");
            BufferBox* b = findBuffer(&r.p, "snd");
            b->replace(counting(3000, 2));
            SoundDoc bd;
            bd.bindBuffer(&r.p, "snd");
            std::weak_ptr<const SampleBuffer> played = bd.sound();
            uint64_t bid = pp.play(bd.sound(), 0, 0, true);
            waitBlocks(5);
            b->message(0, {Atom("clear")});
            CHECK(bd.followBuffer());
            b->message(0, {Atom("size"), Atom(10.f)});
            waitBlocks(5);
            CHECK(pp.active(bid) && !played.expired());   // what it plays is its own snapshot
            last = doc->sound();                         // the edited sound, played as the editor closes
            id = pp.play(doc->sound(), 0, 0, true);
        }
        waitBlocks(5);
        doc.reset();                                     // the editor closed while playing
        waitBlocks(10);
        CHECK(!last.expired() && pp.active(id));     // the document is gone, its samples play on
        for (int k = 0; k < 30; ++k) {                   // replaced quickly, many times
            auto s = std::make_shared<SampleBuffer>(counting(500, 1));
            pp.play(s, 0, 0, k % 2 == 0);
        }
        waitBlocks(10);
        pp.stop();
    }
    waitBlocks(20);
    run.store(false);
    audio.join();
    pp.collect();
    CHECK(first.expired() && last.expired());         // everything handed back was freed
    CHECK(pp.waiting() == 0);
    CHECK(sum.load() > 0.f);
}
