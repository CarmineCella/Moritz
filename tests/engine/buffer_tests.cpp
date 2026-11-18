// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// buffer_tests.cpp — messages that change a name, a buffer~'s contents or a gain no longer compile
// the whole graph again (every open patch with it): BufferRef looks the name up for that box only,
// buffer~ swaps its contents in under the lock, conv~ prepares aside and swaps

#include "rig.h"

#include <cmath>

using namespace moritz;
using etest::Rig;

namespace {
// fills a buffer~ with a constant (on the GUI side, as the code box's set-buffer! would)
void fill(Patch& p, const std::string& name, float v, long frames) {
    BufferBox* b = findBuffer(&p, name);
    SampleBuffer s;
    s.frames = frames;
    s.sr = 44100;
    s.data.assign((size_t)frames, v);
    b->replace(std::move(s));
}
}  // namespace

TEST(set_on_play_changes_buffer_without_compiling) {
    Rig r("(patch (obj a buffer~ one 100) (obj b buffer~ two 100) (obj p play~ one) (obj o dac~ 1)"
          " (connect p 0 o 0))");
    fill(r.p, "one", 0.25f, 4410);
    fill(r.p, "two", 0.5f, 4410);
    r.box("p")->message(0, {Atom("start")});
    auto y = etest::channel(r.run(640), 2, 0);
    CHECK(std::fabs(y[100] - 0.25f) < 1e-6f);
    uint64_t before = r.p.compiles;
    r.box("p")->message(0, {Atom("set"), Atom("two")});
    r.box("p")->message(0, {Atom("start")});
    y = etest::channel(r.run(640), 2, 0);
    CHECK(std::fabs(y[100] - 0.5f) < 1e-6f);
    CHECK(r.p.compiles == before);
}

TEST(buffer_clear_and_size_do_not_compile) {
    Rig r("(patch (obj a buffer~ one 100) (obj p play~ one) (obj o dac~ 1) (connect p 0 o 0))");
    fill(r.p, "one", 0.25f, 4410);
    uint64_t before = r.p.compiles;
    findBuffer(&r.p, "one")->message(0, {Atom("size"), Atom(200.f)});
    CHECK(findBuffer(&r.p, "one")->buf.frames == 8820);
    CHECK(findBuffer(&r.p, "one")->buf.data[100] == 0.25f);
    findBuffer(&r.p, "one")->message(0, {Atom("clear")});
    r.box("p")->message(0, {Atom("start")});
    auto y = etest::channel(r.run(640), 2, 0);
    CHECK(y[100] == 0.f);
    CHECK(r.p.compiles == before);
}

// conv~: gain and a buffer~ whose contents change are prepared aside, without compiling
TEST(conv_gain_and_new_response_do_not_compile) {
    Rig r("(patch (obj ir buffer~ ir 10) (obj c click~) (obj v conv~ ir) (obj o dac~ 1)"
          " (connect c 0 v 0) (connect v 0 o 0))");
    r.box("v")->message(0, {Atom("normalize"), Atom(0.f)});
    fill(r.p, "ir", 0.f, 4);
    BufferBox* ir = findBuffer(&r.p, "ir");
    SampleBuffer s;
    s.frames = 4;
    s.data = {0.5f, 0.f, 0.f, 0.f};
    uint64_t before = r.p.compiles;
    ir->replace(std::move(s));
    r.box("c")->message(0, {Atom("bang")});
    auto y = etest::channel(r.run(64), 2, 0);
    CHECK(std::fabs(y[0] - 0.5f) < 1e-6f);
    r.box("v")->message(0, {Atom("gain"), Atom(0.5f)});
    r.box("c")->message(0, {Atom("bang")});
    y = etest::channel(r.run(64), 2, 0);
    CHECK(std::fabs(y[0] - 0.25f) < 1e-6f);
    CHECK(r.p.compiles == before);
}

// set on the other boxes that name a buffer~: no compile either
TEST(set_on_buffer_users_does_not_compile) {
    Rig r("(patch (obj a buffer~ one 100) (obj b buffer~ two 100) (obj g groove~ one) (obj rc record~ one)"
          " (obj i index~ one) (obj k poke~ one) (obj w waveform~ one) (obj sp sparkle~ one)"
          " (obj v conv~ one) (obj n nebula~ one))");
    uint64_t before = r.p.compiles;
    for (const char* id : {"g", "rc", "i", "k", "w", "sp", "v"}) r.box(id)->message(0, {Atom("set"), Atom("two")});
    r.box("sp")->message(0, {Atom("cross"), Atom(1.f), Atom("two")});
    r.box("n")->message(0, {Atom("proto1"), Atom("two")});
    r.run(256);
    CHECK(r.p.compiles == before);
}
