// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// fixes_tests.cpp — one test per fixed bug of the engine and its objects (each fails without its fix)

#include "rig.h"

#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <thread>

using namespace moritz;
using etest::Rig;

namespace {

[[maybe_unused]] float peak(const std::vector<float>& x, size_t from = 0, size_t to = ~size_t(0)) {
    float p = 0.f;
    for (size_t i = from; i < std::min(to, x.size()); ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}

// a box that remembers the messages it gets and the thread they come on
class Probe : public Box {
public:
    Probe() { setPorts({Port::Control}, {}); cls = "probe"; }
    void message(int, const Message& m) override {
        if (!m.empty() && m[0].isFloat()) values.push_back(m[0].f);
        threads.push_back(std::this_thread::get_id());
    }
    std::vector<float> values;
    std::vector<std::thread::id> threads;
};

[[maybe_unused]] std::string tempPath(const std::string& name) {
    const char* t = std::getenv("TMPDIR");
    std::string d = t && *t ? t : "/tmp";
    if (d.back() != '/') d += '/';
    return d + name;
}

}  // namespace
