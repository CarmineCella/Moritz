// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// stress_tests.cpp — the patch edited on this thread (boxes added and removed, cables, buffer~
// names and contents, a whole text applied, a patch loaded, extras opened and closed) while
// another thread runs Patch::process as the audio callback does. Run it under ThreadSanitizer
// (-DMORITZ_SANITIZE=thread): it must be clean. And the audio never goes silent for an edit.

#include "rig.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

using namespace moritz;
using etest::Rig;

namespace {
const char* kPatch =
    "(patch (obj osc cycle~ 440) (obj g *~ 0.1) (obj o dac~ 1 2)"
    " (obj a buffer~ ba 100) (obj b buffer~ bb 100) (obj pl play~ ba) (obj gr groove~ ba)"
    " (obj cv conv~ ba) (obj s sig~ 0.5) (obj snd send~ bus) (obj rcv receive~ bus) (obj ix index~ ba)"
    " (obj t1 tapin~ 100) (obj t2 tapout~ 10) (obj cm comb~ 20 10 1 0 0.5)"
    " (connect osc 0 g 0) (connect g 0 o 0) (connect s 0 gr 0) (connect s 0 snd 0)"
    " (connect g 0 cv 0) (connect g 0 t1 0) (connect t1 0 t2 0) (connect g 0 cm 0))";
}  // namespace

TEST(edits_while_audio_runs) {
    Rig r(kPatch);
    std::atomic<bool> stop{false};
    std::atomic<long> blocks{0}, silent{0};
    std::thread audio([&] {
        std::vector<float> buf(2 * 64);
        while (!stop) {
            r.p.process(nullptr, 0, buf.data(), 2, 64);
            float p = 0.f;
            for (int i = 0; i < 64; ++i) p = std::max(p, std::fabs(buf[(size_t)i * 2]));
            if (p == 0.f) silent.fetch_add(1);
            blocks.fetch_add(1);
        }
    });
    const char* classes[] = {"cycle~", "noise~", "*~", "line~", "click~", "reson~", "biquad~", "pfft~",
                             "metro", "+", "print", "buffer~", "play~", "record~", "sah~", "count~"};
    std::string err;
    for (int k = 0; k < 200; ++k) {
        // a box comes, is wired and goes
        const char* cls = classes[k % 16];
        Message args;
        if (std::string(cls) == "buffer~" || std::string(cls) == "play~" || std::string(cls) == "record~")
            args = {Atom(k % 2 ? "ba" : "bc")};
        Box* b = r.p.addBox(r.p.create(cls, args), 300, 20 * (k % 10));
        if (b->numIns() > 0 && b->ins[0] == Port::Signal) r.p.connect(r.box("g"), 0, b, 0);
        // names and contents
        r.box("pl")->message(0, {Atom("set"), Atom(k % 2 ? "bb" : "ba")});
        r.box("pl")->message(0, {Atom("start")});
        findBuffer(&r.p, "bb")->message(0, {Atom("size"), Atom(50.f + k % 20)});
        if (k % 7 == 0) findBuffer(&r.p, "ba")->message(0, {Atom("clear")});
        r.box("cv")->message(0, {Atom("gain"), Atom(0.5f + 0.01f * (k % 10))});
        r.box("rcv")->message(0, {Atom("set"), Atom(k % 3 ? "bus" : "other")});
        r.p.sendTo("nothing", {Atom((float)k)});
        r.p.removeBoxes({b});
        // a whole text applied (the boxes that did not change stay), an extra patch comes and goes
        if (k % 10 == 0) {
            std::string text = r.p.serialize();
            if (!r.p.applyText(text, err)) std::printf("  applyText: %s\n", err.c_str());
            auto xp = std::make_unique<Patch>();
            Patch* x = r.p.addExtra(std::move(xp));
            x->load("(patch (obj n noise~) (obj d dac~ 2) (connect n 0 d 0))", err);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            r.p.removeExtra(x);
        }
        if (k % 50 == 49) {                           // the whole patch again
            if (!r.p.load(kPatch, err)) std::printf("  load: %s\n", err.c_str());
            r.p.loadbangAll();
        }
    }
    stop = true;
    audio.join();
    std::printf("  %ld blocks while editing, %ld silent\n", blocks.load(), silent.load());
    CHECK(blocks.load() > 0);
    CHECK(silent.load() == 0);             // an edit never silences the audio (it used to: try_lock)
}

// line~ lists sent while the audio thread starts them: a new list replaces the one in use, which goes back
// to this thread to be freed, so the audio thread must be done reading it (the jumps left at its head were
// read after it was handed back). Under ThreadSanitizer: clean.
TEST(line_lists_while_audio_runs) {
    Rig r("(patch (obj ln line~) (obj o dac~ 1 2) (connect ln 0 o 0))");
    std::atomic<bool> stop{false};
    std::atomic<long> blocks{0};
    std::thread audio([&] {
        std::vector<float> buf(2 * 16);
        while (!stop) {
            r.p.process(nullptr, 0, buf.data(), 2, 16);
            blocks.fetch_add(1);
        }
    });
    Message jumps;                                    // 100 jumps: one a sample, so a list lasts 100 samples
    for (int k = 0; k < 100; ++k) { jumps.push_back(Atom((float)(k % 2))); jumps.push_back(Atom(0.f)); }
    for (int k = 0; k < 60000; ++k) {
        r.box("ln")->message(0, jumps);
        if (k % 4 == 0) std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
    stop = true;
    audio.join();
    CHECK(blocks.load() > 0);
}
