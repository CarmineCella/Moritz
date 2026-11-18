// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// hop_tests.cpp — live analyses (descriptors~, melbands~ ...) analyse frames that end every hop
// samples exactly, wherever the ring is when the GUI's timer comes

#include "rig.h"

#include <cmath>

using namespace moritz;
using etest::Rig;

namespace {
class Collect : public Box {
public:
    Collect() { setPorts({Port::Control}, {}); cls = "collect"; }
    void message(int, const Message& m) override {
        if (!m.empty() && m[0].isFloat()) values.push_back(m[0].f);
    }
    std::vector<float> values;
};
}  // namespace

// a ramp from 0 to 1 over a second: the rms of each frame tells where the frame ended
TEST(analysis_frames_end_on_hops) {
    Rig r("(patch (obj l line~) (obj d descriptors~ 2048 1024 rms) (connect l 0 d 0))");
    auto c = std::make_unique<Collect>();
    Collect* col = c.get();
    r.p.connect(r.box("d"), 0, r.p.addBox(std::move(c), 0, 100), 0);
    r.box("l")->message(0, {Atom(0.f), Atom(0.f), Atom(1.f), Atom(1000.f)});
    for (int k = 0; k < 40; ++k) r.run(1000 + 37 * (k % 5), 500 + 13 * (k % 7));   // odd buffers
    CHECK(col->values.size() >= 30);
    bool aligned = !col->values.empty();
    for (size_t k = 0; k < col->values.size(); ++k) {
        long end = (long)(k + 1) * 1024;              // the frame ends on its hop
        double s = 0;
        for (long i = end - 2048; i < end; ++i) {
            double x = i <= 0 ? 0.0 : std::min(1.0, (double)i / 44100.0);
            s += x * x;
        }
        double want = std::sqrt(s / 2048);
        if (std::fabs(col->values[k] - want) > 2e-4 + 1e-3 * want) {
            std::printf("  frame %zu: %g, want %g\n", k, col->values[k], want);
            aligned = false;
            break;
        }
    }
    CHECK(aligned);
}
