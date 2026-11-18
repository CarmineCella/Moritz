// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// rate_tests.cpp — what boxes prepare for the sample rate is prepared at compile, for the rate the
// patch runs at, and again when the rate changes: never in dsp()

#include "rig.h"

#include <cmath>

using namespace moritz;
using etest::Rig;

// comb~ (and tapin~, allpass~) were sized for 96 kHz: at 192 kHz a 49 ms delay of a 50 ms comb~
// was cut to about 25 ms. Now they are sized for the rate they run at.
TEST(comb_is_sized_for_the_rate) {
    Rig r("(patch (obj c click~) (obj k comb~ 50 49 0 1 0) (obj o dac~ 1) (connect c 0 k 0) (connect k 0 o 0))",
          192000);
    r.box("c")->message(0, {Atom("bang")});
    auto y = etest::channel(r.run(192000 / 10), 2, 0);
    size_t first = 0;
    for (size_t i = 0; i < y.size() && !first; ++i)
        if (std::fabs(y[i]) > 0.5f) first = i;
    CHECK(first >= 9400 && first <= 9420);    // 49 ms at 192 kHz: 9408 samples
}

// tapin~ / tapout~ too: a 900 ms tap of a 1000 ms line at 192 kHz
TEST(tapin_is_sized_for_the_rate) {
    Rig r("(patch (obj c click~) (obj ti tapin~ 1000) (obj to tapout~ 900) (obj o dac~ 1)"
          " (connect c 0 ti 0) (connect ti 0 to 0) (connect to 0 o 0))",
          192000);
    r.box("c")->message(0, {Atom("bang")});
    auto y = etest::channel(r.run(192000), 2, 0);
    size_t first = 0;
    for (size_t i = 0; i < y.size() && !first; ++i)
        if (std::fabs(y[i]) > 0.5f) first = i;
    CHECK(first >= 172790 && first <= 172810);    // 900 ms at 192 kHz: 172800 samples
}

// hrtf~ and binaural~ prepared themselves in dsp() when the rate changed (allocating, and taking a
// lock the GUI takes). Now the compile after the rate change prepares them, before audio runs.
TEST(head_responses_are_prepared_when_the_rate_changes) {
    Rig r("(patch (obj n noise~) (obj h hrtf~ 30 0) (obj b binaural~ 1) (obj o dac~ 1 2)"
          " (connect n 0 h 0) (connect n 0 b 0) (connect n 0 b 1) (connect n 0 b 3)"
          " (connect h 0 o 0) (connect b 1 o 1))");
    r.run(4410);
    r.p.sampleRate = 48000.0;
    r.sr = 48000;
    r.p.rateChanged();
    long before = etest::audioAllocations();
    auto out = r.run(4800);
    if (etest::allocGuardActive()) CHECK(etest::audioAllocations() == before);
    CHECK(etest::peakOf(etest::channel(out, 2, 0)) > 0.f);
    CHECK(etest::peakOf(etest::channel(out, 2, 1)) > 0.f);
}
