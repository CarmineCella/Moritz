// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// bench_tests.cpp — what a block costs (printed, not checked: machines differ). binaural~ at order 5
// with its direct filters, against the same filters run by Musil's PartConv<float> (B = 256: an
// impulse response of 139 taps is all head, applied directly over 256 taps).

#include "rig.h"
#include "signals/convolution.h"

#include <chrono>

using namespace moritz;
using etest::Rig;

TEST(bench_binaural_order5) {
    Rig r("(patch (obj n noise~) (obj b binaural~ 5) (obj o dac~ 1 2) (connect b 0 o 0) (connect b 1 o 1))", 48000);
    for (int k = 0; k < 36; ++k) r.p.connect(r.box("n"), 0, r.box("b"), k);
    r.run(4800);
    auto t0 = std::chrono::steady_clock::now();
    const int blocks = 2000;
    r.run(64L * blocks);
    double direct = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;

    // the same work through PartConv: 36 channels x 2 ears, 139 taps each
    std::vector<musil::dsp::PartConv<float>> pc(72);
    std::vector<float> ir(139, 0.01f), x(64, 0.1f), y(64);
    volatile float sink = 0.f;   // the results are used: nothing optimised away
    for (auto& c : pc) c.setIR(ir.data(), (long)ir.size(), 1, 1.f);
    t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b)
        for (auto& c : pc) { c.process(x.data(), y.data(), 64); sink += y[63]; }
    double part = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
    std::printf("  binaural~ 5 (36 channels) per 64-sample block: direct %.1f us, PartConv %.1f us (a block lasts %.0f us)\n",
                direct, part, 64.0 / 48000 * 1e6);
    CHECK(direct > 0 && sink != 12345.f);
}
