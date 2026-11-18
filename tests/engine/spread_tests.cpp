// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// spread_tests.cpp — sparkle~ computes its frames on a worker thread while audio plays (one hop
// later), and the result is the one rendered offline, where frames are computed at once

#include "rig.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <cmath>
#include <thread>

using namespace moritz;
using etest::Rig;

namespace {
const char* kPatch = "(patch (obj c cycle~ 330) (obj s sparkle~) (obj o dac~ 1) (connect c 0 s 0) (connect s 0 o 0))";
}

// pfft~ spreads a frame's work over the next hop: a click through a pass-through pfft~ 512 4
// comes out fftsize + hop = 640 samples later, whole
TEST(pfft_latency_is_fftsize_and_a_hop) {
    const char* t = std::getenv("TMPDIR");
    std::string dir = t && *t ? t : "/tmp";
    {
        std::ofstream f(dir + "/moritz-engine-pass.moritz");
        f << "(patch (obj i fftin~ 1) (obj o fftout~ 1) (connect i 0 o 0) (connect i 1 o 1))\n";
    }
    Rig r("");
    r.p.dir = dir;
    std::string err;
    r.p.load("(patch (obj c click~) (obj f pfft~ moritz-engine-pass 512 4) (obj o dac~ 1)"
             " (connect c 0 f 0) (connect f 0 o 0))", err);
    r.run(2048);
    r.box("c")->message(0, {Atom("bang")});
    auto y = etest::channel(r.run(4096), 2, 0);
    size_t at = 0;
    float best = 0.f;
    for (size_t i = 0; i < y.size(); ++i)
        if (std::fabs(y[i]) > best) { best = std::fabs(y[i]); at = i; }
    CHECK(at == 640);
    CHECK(std::fabs(best - 1.f) < 0.01f);
    std::remove((dir + "/moritz-engine-pass.moritz").c_str());
}

TEST(sparkle_worker_matches_offline) {
    Rig off(kPatch);
    auto a = etest::channel(off.run(22050), 2, 0);

    Rig live("");
    live.p.offline = false;                    // as with audio on: the frames go to a worker thread
    std::string err;
    live.p.load(kPatch, err);
    std::vector<float> b, buf(2 * 64);
    Box* s = live.box("s");
    bool finished = true;
    for (int k = 0; k < 22050 / 64 + 1; ++k) {
        // each frame is given the time it needs (a machine under load, or a sanitizer, is slower than real
        // time: a frame late is skipped by design, which is not what this compares)
        auto t0 = std::chrono::steady_clock::now();
        while (s->working() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10))
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        finished = finished && !s->working();
        live.p.process(nullptr, 0, buf.data(), 2, 64);
        for (int i = 0; i < 64; ++i) b.push_back(buf[(size_t)i * 2]);
    }
    CHECK(finished);
    double worst = 0, peak = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        worst = std::max(worst, (double)std::fabs(a[i] - b[i]));
        peak = std::max(peak, (double)std::fabs(a[i]));
    }
    CHECK(peak > 0.01);
    CHECK(worst < 1e-5);
}
