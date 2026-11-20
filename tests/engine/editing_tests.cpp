// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// editing_tests.cpp — edits the patch editor makes with the mouse, without a window: a cable's end moved
// to another port, a box dropped into a cable

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

// A box dropped into a cable: A -> B becomes A -> box -> B, where the cable was; nothing changes when the
// box can't take it
TEST(insert_into_cable_splices_a_box_or_changes_nothing) {
    Rig r("(patch (obj osc cycle~ 440) (obj d dac~ 1) (obj g *~ 0.5) (obj f +~ 1) (obj n + 1) (obj m metro 100)"
          " (obj b print) (obj q print) (connect osc 0 d 0) (connect m 0 b 0) (connect g 0 f 0))");
    Patch& p = r.p;
    std::string err;
    CHECK(p.insertIntoCable(0, r.box("g"), &err));                          // a signal box into a signal cable
    CHECK(cables(p) == "osc 0 g 0|g 0 d 0|m 0 b 0|g 0 f 0|");
    std::string before = cables(p);
    CHECK(!p.insertIntoCable(2, r.box("f"), &err));                         // a signal outlet into print's inlet
    CHECK(err.find("control inlet") != std::string::npos);
    CHECK(cables(p) == before);
    CHECK(!p.insertIntoCable(0, r.box("n"), &err));                         // the signal into a control inlet
    CHECK(cables(p) == before);
    CHECK(!p.insertIntoCable(2, r.box("q"), &err));                         // print has no outlet
    CHECK(err.find("an inlet and an outlet") != std::string::npos);
    CHECK(cables(p) == before);
    CHECK(p.insertIntoCable(2, r.box("n"), &err));                          // a control box into a control cable
    CHECK(cables(p) == "osc 0 g 0|g 0 d 0|m 0 n 0|n 0 b 0|g 0 f 0|");
    before = cables(p);
    CHECK(!p.insertIntoCable(0, r.box("f"), &err));                         // osc -> f -> g while g -> f: a loop
    CHECK(err.find("loop") != std::string::npos);
    CHECK(cables(p) == before);
    CHECK(!p.insertIntoCable(1, r.box("g"), &err));                         // g is at an end of g -> d
    CHECK(cables(p) == before);
    r.run(256);
}
