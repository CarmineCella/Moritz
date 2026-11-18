// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// behaviour_tests.cpp — objects that now behave as they should (0.11: they sound different than before)

#include "rig.h"

#include <cmath>

using namespace moritz;
using etest::Rig;

// lrshift~: a number on its inlet sets the shift (it used to set the signal's value)
TEST(lrshift_takes_its_shift_from_a_number) {
    Rig r("(patch (obj s sig~ 1) (obj l lrshift~) (obj o dac~ 1) (connect s 0 l 0) (connect l 0 o 0))");
    r.box("l")->message(0, {Atom(3.f)});
    auto y = etest::channel(r.run(128), 2, 0);
    CHECK(y[0] == 0.f && y[2] == 0.f && y[3] == 1.f);
    CHECK(y[64] == 0.f && y[66] == 0.f && y[67] == 1.f);
}
