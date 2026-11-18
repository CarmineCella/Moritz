// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// rate_tests.cpp — what boxes prepare for the sample rate is prepared at compile, for the rate the
// patch runs at, and again when the rate changes: never in dsp()

#include "rig.h"

#include <cmath>

using namespace moritz;
using etest::Rig;

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
