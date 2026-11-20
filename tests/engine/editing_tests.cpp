// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// editing_tests.cpp — edits the patch editor makes with the mouse, without a window: a cable's end moved
// to another port

#include "rig.h"

using namespace moritz;
using etest::Rig;

namespace {

std::string cables(const Patch& p) {
    std::string s;
    for (auto& c : p.conns)
        s += c.from->id + " " + std::to_string(c.outlet) + " " + c.to->id + " " + std::to_string(c.inlet) + "|";
    return s;
}

}  // namespace

// A cable given another end keeps its place among the cables; one that can't be made changes nothing
TEST(reconnect_moves_a_cable_and_keeps_its_place) {
    Rig r("(patch (obj osc cycle~ 440) (obj g *~ 0.5) (obj h *~ 1) (obj d dac~ 1) (obj m metro 100)"
          " (connect osc 0 g 0) (connect g 0 h 0) (connect h 0 d 0))");
    Patch& p = r.p;
    std::string err;
    CHECK(p.reconnect(1, r.box("g"), 0, r.box("d"), 0, &err));          // its inlet end, to the dac~
    CHECK(cables(p) == "osc 0 g 0|g 0 d 0|h 0 d 0|");
    CHECK(p.reconnect(1, r.box("osc"), 0, r.box("d"), 0, &err));        // its outlet end, to the oscillator
    CHECK(cables(p) == "osc 0 g 0|osc 0 d 0|h 0 d 0|");
    std::string before = cables(p);
    CHECK(!p.reconnect(1, r.box("osc"), 0, r.box("m"), 0, &err));       // a signal into a control inlet
    CHECK(err.find("control inlet") != std::string::npos);
    CHECK(cables(p) == before);
    CHECK(p.connect(r.box("g"), 0, r.box("h"), 0, &err));
    before = cables(p);
    CHECK(!p.reconnect(2, r.box("h"), 0, r.box("g"), 1, &err));         // h -> g while g -> h: a loop
    CHECK(err.find("loop") != std::string::npos);
    CHECK(cables(p) == before);
    CHECK(!p.reconnect(1, r.box("osc"), 0, r.box("g"), 0, &err));       // there is one already
    CHECK(cables(p) == before);
    CHECK(!p.reconnect(9, r.box("osc"), 0, r.box("d"), 0, &err));
    CHECK(cables(p) == before);
    r.run(256);                                                         // the plan runs
}
