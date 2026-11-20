// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// descriptors_tests.cpp — descriptors~ on the live-analysis frames: YIN's f0 (a sawtooth, a missing or weak
// fundamental, noise, the range), its lists (the same as melbands~, mfcc~ and chroma~ at its size), and the
// same numbers as Musil's descriptors of the same samples

#include "rig.h"
#include "mir.h"

#include <cmath>

using namespace moritz;
using etest::Rig;

namespace {
// keeps every message of every inlet
class Keep : public Box {
public:
    explicit Keep(int inlets) {
        setPorts(std::vector<Port>((size_t)inlets, Port::Control), {});
        cls = "keep";
        got.resize((size_t)inlets);
    }
    void message(int in, const Message& m) override { got[(size_t)in].push_back(m); }
    std::vector<std::vector<Message>> got;
};
Keep* keep(Rig& r, const std::string& from, int outlets) {
    auto k = std::make_unique<Keep>(outlets);
    Keep* p = k.get();
    Box* b = r.p.addBox(std::move(k), 0, 300);
    for (int o = 0; o < outlets; ++o) r.p.connect(r.box(from), o, b, o);
    return p;
}
// the median of the last n values of an outlet (one frame may straddle a change)
float lastMedian(const std::vector<Message>& v, size_t n) {
    std::vector<float> x;
    for (size_t k = v.size() > n ? v.size() - n : 0; k < v.size(); ++k)
        if (!v[k].empty() && v[k][0].isFloat()) x.push_back(v[k][0].f);
    if (x.empty()) return -1.f;
    std::sort(x.begin(), x.end());
    return x[x.size() / 2];
}
double cents(double f, double want) { return f > 0 ? std::fabs(1200 * std::log2(f / want)) : 1e9; }
}  // namespace

// a band-limited sawtooth at 220 Hz (saw~): f0 within a cent, periodic
TEST(descriptors_f0_of_a_sawtooth) {
    Rig r("(patch (obj s saw~ 220) (obj d descriptors~ 2048 1024 f0 periodicity) (connect s 0 d 0))");
    Keep* k = keep(r, "d", 2);
    r.run(44100);
    float f0 = lastMedian(k->got[0], 9), per = lastMedian(k->got[1], 9);
    std::printf("  saw~ 220: f0 %.3f, periodicity %.4f\n", f0, per);
    CHECK(k->got[0].size() >= 40);
    CHECK(cents(f0, 220) < 1);
    CHECK(per > .95f);
}

// 400 + 600 + 800 Hz: the fundamental, 200 Hz, is missing; YIN finds it
TEST(descriptors_f0_missing_fundamental) {
    Rig r("(patch (obj a cycle~ 400) (obj b cycle~ 600) (obj c cycle~ 800) (obj d descriptors~ 2048 1024 f0)"
          " (connect a 0 d 0) (connect b 0 d 0) (connect c 0 d 0))");
    Keep* k = keep(r, "d", 1);
    r.run(44100);
    float f0 = lastMedian(k->got[0], 9);
    std::printf("  400 + 600 + 800 Hz: f0 %.3f\n", f0);
    CHECK(cents(f0, 200) < 1);
}

// 110 Hz with a weak fundamental and a strong octave: the autocorrelation descriptors~ used before 0.11 said
// 220.5 Hz (an octave up); YIN says 110
TEST(descriptors_f0_weak_fundamental) {
    std::string p = "(patch (obj d descriptors~ 2048 1024 f0)";
    const float amps[6] = {.1f, 1.f, .3f, .8f, .3f, .5f};
    for (int h = 1; h <= 6; ++h) {
        std::string o = "o" + std::to_string(h), g = "g" + std::to_string(h);
        p += " (obj " + o + " cycle~ " + std::to_string(110 * h) + ") (obj " + g + " *~ " + std::to_string(amps[h - 1] * .3f) + ")";
        p += " (connect " + o + " 0 " + g + " 0) (connect " + g + " 0 d 0)";
    }
    Rig r(p + ")");
    Keep* k = keep(r, "d", 1);
    r.run(44100);
    float f0 = lastMedian(k->got[0], 9);
    std::printf("  110 Hz, weak fundamental: f0 %.3f\n", f0);
    CHECK(cents(f0, 110) < 1);
}

// noise is unvoiced; a range above the note finds no period in it; threshold 1 calls anything voiced
TEST(descriptors_f0_unvoiced_and_options) {
    Rig r("(patch (obj n noise~) (obj d descriptors~ 2048 1024 f0) (connect n 0 d 0)"
          " (obj s saw~ 110) (obj e descriptors~ 2048 1024 f0) (connect s 0 e 0))");
    Keep* kn = keep(r, "d", 1);
    Keep* ks = keep(r, "e", 1);
    r.run(22050);
    CHECK(lastMedian(kn->got[0], 9) == 0.f);
    CHECK(cents(lastMedian(ks->got[0], 9), 110) < 1);
    r.box("e")->message(0, {Atom("range"), Atom(150.f), Atom(4000.f)});
    r.run(22050);
    float ranged = lastMedian(ks->got[0], 5);
    std::printf("  saw~ 110 with range 150 4000: f0 %.3f\n", ranged);
    CHECK(ranged == 0.f || cents(ranged, 220) < 5);
    CHECK(cents(ranged, 110) > 100);
    r.box("d")->message(0, {Atom("threshold"), Atom(1.f)});
    r.run(22050);
    CHECK(lastMedian(kn->got[0], 5) > 0.f);
}

// the lists are melbands~'s, mfcc~'s and chroma~'s at the same size, frame for frame
TEST(descriptors_lists_match_the_live_objects) {
    Rig r("(patch (obj s saw~ 330) (obj d descriptors~ 2048 512 melbands mfcc chroma rms)"
          " (obj m melbands~ 40 2048 512) (obj c mfcc~ 13 40 2048 512) (obj h chroma~ 2048 512)"
          " (connect s 0 d 0) (connect s 0 m 0) (connect s 0 c 0) (connect s 0 h 0))");
    Keep* kd = keep(r, "d", 4);
    Keep* km = keep(r, "m", 1);
    Keep* kc = keep(r, "c", 1);
    Keep* kh = keep(r, "h", 1);
    r.run(22050);
    CHECK(kd->got[0].size() >= 30);
    bool same = kd->got[0].size() == km->got[0].size() && kd->got[1].size() == kc->got[0].size() &&
                kd->got[2].size() == kh->got[0].size();
    CHECK(same);
    if (!same) return;
    CHECK(kd->got[0].back().size() == 40);
    CHECK(kd->got[1].back().size() == 13);
    CHECK(kd->got[2].back().size() == 12);
    double worst = 0;
    for (size_t f = 0; f < kd->got[0].size(); ++f)
        for (int w = 0; w < 3; ++w) {
            const Message& a = kd->got[(size_t)w][f];
            const Message& b = (w == 0 ? km : w == 1 ? kc : kh)->got[0][f];
            if (a.size() != b.size()) { worst = 1e9; continue; }
            for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, (double)std::fabs(a[i].f - b[i].f));
        }
    std::printf("  lists against melbands~ mfcc~ chroma~: largest difference %g\n", worst);
    CHECK(worst == 0);
}

// frame k ends at (k + 1) hop samples: Musil's descriptors (frames centred, the first at 0) gives the same
// numbers for hop = size / 2, the scalars and YIN's f0 alike
TEST(descriptors_match_musil) {
    Rig r("(patch (obj l line~) (obj s saw~ 180) (obj m *~) (obj d descriptors~ 2048 1024)"
          " (connect s 0 m 0) (connect l 0 m 1) (connect m 0 d 0))");
    Keep* k = keep(r, "d", mir::kDescriptorCount);
    r.box("l")->message(0, {Atom(0.f), Atom(0.f), Atom(1.f), Atom(500.f)});   // a fade in: frames differ
    r.run(44100);
    size_t frames = k->got[0].size();
    CHECK(frames >= 40);
    // the signal descriptors~ saw: the same patch again, into dac~
    Rig q("(patch (obj l line~) (obj s saw~ 180) (obj m *~) (obj o dac~)"
          " (connect s 0 m 0) (connect l 0 m 1) (connect m 0 o 0))");
    q.box("l")->message(0, {Atom(0.f), Atom(0.f), Atom(1.f), Atom(500.f)});
    std::vector<float> x = etest::channel(q.run(44100), 2, 0);
    mir::FrameDescriptors fd;
    fd.init(2048, 44100.0);
    mir::FrameSpectrum fs;
    fs.init(2048);
    std::vector<double> buf(2048);
    mir::Vec mag;
    double v[mir::kDescriptorCount], worst = 0;
    int worstAt = -1;
    for (size_t f = 0; f < frames; ++f) {
        long c = (long)f * 1024;                                 // Musil's frame f, centred on f * hop
        for (long i = 0; i < 2048; ++i) {
            long j = c - 1024 + i;
            buf[(size_t)i] = j >= 0 && j < (long)x.size() ? x[(size_t)j] : 0.0;
        }
        fs.magnitudes(buf.data(), mag);
        fd.compute(buf.data(), mag, v);
        for (int d = 0; d < mir::kDescriptorCount; ++d) {
            double got = k->got[(size_t)d][f][0].f, want = v[d];
            double err = std::fabs(got - want) / (1e-6 + std::fabs(want));
            if (err > worst) { worst = err; worstAt = d; }
        }
    }
    std::printf("  against Musil's descriptors: largest relative difference %g (%s)\n", worst,
                worstAt >= 0 ? mir::descriptorName(worstAt) : "-");
    CHECK(worst < 1e-4);
}
