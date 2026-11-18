// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// expr_tests.cpp — expr and expr~ compile to a flat program: the same values as before (Max's typing:
// integer operations between integers), and what a sample of expr~ costs

#include "expr.h"
#include "rig.h"

#include <chrono>
#include <cmath>

using namespace moritz;
using etest::Rig;

TEST(expr_program_values) {
    struct Case {
        const char* src;
        double v1, v2, want;
    } cases[] = {
        {"$f1 * 2 + sin($f2)", 1.5, 0.5, 3.0 + std::sin(0.5)},
        {"7 / 2", 0, 0, 3},
        {"7. / 2", 0, 0, 3.5},
        {"$i1 % 3", 7.9, 0, 1},
        {"$f1 / 0", 4, 0, 0},
        {"($f1 > 1) && ($f2 <= 2) || 0", 2, 2, 1},
        {"!($f1 == $f2)", 3, 3, 0},
        {"max(1, 2.5) - min($f1, $f2)", 4, -1, 3.5},
        {"-(3) + pow(2, 10)", 0, 0, 1021},
        {"int(2.7) + floor(-0.5) + ceil(0.2) + round(2.5)", 0, 0, 2 - 1 + 1 + 3},
        {"atan2(1, 1) * 4 - pi", 0, 0, 0},
        {"fmod($f1, 0)", 5, 0, 0},
    };
    bool all = true;
    for (auto& c : cases) {
        ExprProgram p;
        int nv = 0;
        std::string err;
        double v[9] = {c.v1, c.v2};
        bool ok = ExprParser(c.src).parse(p, nv, err);
        double got = ok ? p.eval(v) : NAN;
        // the block evaluation (expr~) gives the same
        float in1[3] = {(float)c.v1, (float)c.v1, (float)c.v1}, in2[3] = {(float)c.v2, (float)c.v2, (float)c.v2}, out[3];
        const float* ins[9] = {in1, in2, in1, in1, in1, in1, in1, in1, in1};
        if (ok) p.evalBlock(ins, 3, out);
        if (!ok || std::fabs(got - c.want) > 1e-12 || std::fabs(out[2] - (float)c.want) > 1e-5f) {
            std::printf("  %s: %g, want %g %s\n", c.src, got, c.want, err.c_str());
            all = false;
        }
    }
    CHECK(all);
    ExprProgram p;
    int nv;
    std::string err;
    CHECK(!ExprParser("1 +").parse(p, nv, err) && !err.empty());
    CHECK(!ExprParser("foo(1)").parse(p, nv, err));
    std::string deep(300, '(');
    CHECK(!ExprParser(deep + "1" + std::string(300, ')')).parse(p, nv, err));   // refused, never a crash
    double v[9] = {};
    CHECK(ExprParser("random(2, 3)").parse(p, nv, err) && p.eval(v) >= 2 && p.eval(v) < 3);
}

TEST(bench_expr_sig) {
    etest::Rig r("(patch (obj n noise~) (obj e expr~ \"$v1 * 0.5 + sin($v1 * 3.) * cos($v2) / (1. + abs($v1))\")"
                 " (obj o dac~ 1) (connect n 0 e 0) (connect n 0 e 1) (connect e 0 o 0))");
    r.run(4410);
    auto t0 = std::chrono::steady_clock::now();
    r.run(441000);
    double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / 441000;
    std::printf("  expr~ (8 operations, 3 calls): %.1f ns a sample, with noise~ and dac~\n", ns);
    CHECK(ns > 0);
}

// random() in expr~ and expr was seeded from std::random_device: a render was never the same twice.
// Now the seed comes from the box's id (the same at each run, another for another box) or `seed n`.
TEST(expr_random_is_the_same_at_each_run) {
    const char* text = "(patch (obj e expr~ \"random(-1., 1.)\") (obj f expr~ \"random(-1., 1.)\") (obj o dac~ 1 2)"
                       " (connect e 0 o 0) (connect f 0 o 1))";
    Rig a(text), b(text);
    auto ya = a.run(256), yb = b.run(256);
    CHECK(ya == yb);
    CHECK(etest::channel(ya, 2, 0) != etest::channel(ya, 2, 1));
    CHECK(etest::peakOf(ya) > 0.5f);
    // seed n: the same numbers from then on
    Rig c(text), d(text);
    c.box("e")->message(0, {Atom("seed"), Atom(7.f)});
    auto yc = etest::channel(c.run(256), 2, 0);
    d.run(512);
    d.box("e")->message(0, {Atom("seed"), Atom(7.f)});
    auto yd = etest::channel(d.run(256), 2, 0);
    CHECK(yc == yd);
    // expr, at control rate
    const char* ctl = "(patch (obj e expr \"random(0, 1000)\") (obj p print v) (connect e 0 p 0))";
    Rig g(ctl), h(ctl);
    for (int k = 0; k < 5; ++k) {
        g.box("e")->message(0, {Atom("bang")});
        h.box("e")->message(0, {Atom("bang")});
    }
    CHECK(g.printed.size() == 5 && g.printed == h.printed);
}
