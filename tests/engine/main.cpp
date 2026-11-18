// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// main.cpp — runs the engine tests (all, or the ones named): moritz-engine-tests [name ...]
// Run from the source folder (ctest does), so the help patches and the data are found.

#include "rig.h"
#include "gui.h"

#include <cstring>

namespace etest {
std::vector<Test>& tests() {
    static std::vector<Test> t;
    return t;
}
int failures = 0;
}  // namespace etest

int main(int argc, char** argv) {
    moritz::setupSearchPaths();
    int ran = 0, failed = 0;
    for (auto& t : etest::tests()) {
        bool wanted = argc < 2;
        for (int k = 1; k < argc; ++k) wanted = wanted || std::strcmp(argv[k], t.name) == 0;
        if (!wanted) continue;
        int before = etest::failures;
        std::printf("%s\n", t.name);
        std::fflush(stdout);
        t.fn();
        ++ran;
        if (etest::failures != before) ++failed;
    }
    std::printf("%d engine tests, %d failed\n", ran, failed);
    return failed == 0 ? 0 : 1;
}
