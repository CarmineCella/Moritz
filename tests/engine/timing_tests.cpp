// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// timing_tests.cpp — timestamped events: a click~ driven by a metro lands on the right sample,
// rendered offline and in real time (where the GUI's timers fire late and the device's buffers
// are long), and a metro never drifts

#include "rig.h"

#include <cmath>
#include <random>

using namespace moritz;
using etest::Rig;

namespace {
std::vector<long> clicks(const std::vector<float>& y) {
    std::vector<long> at;
    for (size_t i = 0; i < y.size(); ++i)
        if (y[i] > 0.5f) at.push_back((long)i);
    return at;
}
}  // namespace

// Offline: every 100 ms tick of a metro is a click exactly 4410 samples after the one before
// (the scheduler used to run at block starts: clicks moved to the next 64-sample block, and the
// metro drifted by what each tick was late)
TEST(metro_clicks_land_on_their_sample_offline) {
    Rig r("(patch (obj l loadbang) (obj m metro 100) (obj c click~) (obj o dac~ 1)"
          " (connect l 0 m 0) (connect m 0 c 0) (connect c 0 o 0))");
    auto at = clicks(etest::channel(r.run(44100), 2, 0));
    CHECK(at.size() == 10);
    bool exact = at.size() >= 2;
    for (size_t k = 0; k < at.size(); ++k) exact = exact && at[k] == (long)k * 4410;
    CHECK(exact);
}

// Real time: the device asks for 512 frames at a time, the GUI fires the metro's timer up to
// 400 samples late at random moments between the callbacks: the clicks still come exactly 4410
// samples apart (two buffers after their time)
TEST(metro_clicks_land_on_their_sample_in_real_time) {
    const int P = 512, sr = 44100;
    double wall = 0;
    Rig r("");
    r.p.offline = false;
    r.p.clock = [&wall] { return wall; };
    r.p.ioBufferFrames = P;
    std::string err;
    r.p.load("(patch (obj l loadbang) (obj m metro 100) (obj c click~) (obj o dac~ 1)"
             " (connect l 0 m 0) (connect m 0 c 0) (connect c 0 o 0))", err);
    std::vector<float> out, buf((size_t)P * 2);
    uint32_t rng = 7;
    auto late = [&rng] { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (int)(rng % 401); };
    long S = 0;
    // the first callback anchors the clock; then the loadbang starts the metro
    wall = 0;
    r.p.process(nullptr, 0, buf.data(), 2, P);
    S += P;
    r.p.loadbangAll();
    for (int k = 0; k < 120; ++k) {
        // the GUI's timers between this callback and the next, each fired late
        double due = r.p.nextDue();
        while (due * sr < S + P) {
            double fireAt = due + late() / (double)sr;
            if (fireAt * sr >= S + P) break;           // it fires after the next callback
            wall = fireAt;
            r.p.runDue(wall);
            due = r.p.nextDue();
        }
        wall = (double)S / sr;                         // the callback for [S, S + P)
        r.p.process(nullptr, 0, buf.data(), 2, P);
        out.insert(out.end(), buf.begin(), buf.end());
        S += P;
        wall = (double)(S - 1) / sr;
        r.p.runDue(wall);                              // and those due just before it
    }
    auto at = clicks(etest::channel(out, 2, 0));
    CHECK(at.size() >= 12);
    bool even = at.size() >= 2;
    for (size_t k = 1; k < at.size(); ++k) even = even && at[k] - at[k - 1] == 4410;
    CHECK(even);
}

// A number on a signal inlet changes the value at its sample: sig~ driven by a metro of 10 ms
TEST(signal_values_change_on_their_sample) {
    Rig r("(patch (obj l loadbang) (obj m metro 10) (obj c counter) (obj g * 0.001) (obj s sig~) (obj o dac~ 1)"
          " (connect l 0 m 0) (connect m 0 c 0) (connect c 0 g 0) (connect g 0 s 0) (connect s 0 o 0))");
    auto y = etest::channel(r.run(4410), 2, 0);
    bool steps = true;
    for (size_t i = 1; i < y.size(); ++i)
        if (y[i] != y[i - 1]) steps = steps && i % 441 == 0;
    CHECK(steps);
}
