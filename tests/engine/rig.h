// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// rig.h — the engine tests' small framework: TEST(name) { ... CHECK(...) ... },
// and Rig, a patch made from text and run the way moritz --render runs one
// (a sample clock, the timers due before each block, then the block).
#pragma once

#include "jobs.h"
#include "objects.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace etest {

struct Test {
    const char* name;
    void (*fn)();
};
std::vector<Test>& tests();
struct Reg {
    Reg(const char* n, void (*f)()) { tests().push_back({n, f}); }
};
extern int failures;

#define TEST(name)                                     \
    static void name();                                \
    static etest::Reg reg_##name(#name, name);         \
    static void name()

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++etest::failures;                                                        \
        }                                                                             \
    } while (0)

// allocations and frees made inside Patch::process (alloc_guard.cpp)
long audioAllocations();
bool allocGuardActive();     // false under the sanitizers, whose runtimes own operator new

struct Rig {
    moritz::Patch p;
    long frame = 0;
    int sr;
    int ch = 2;
    std::vector<std::string> printed;
    std::string error;

    explicit Rig(const std::string& text, int sampleRate = 44100) : sr(sampleRate) {
        p.onPrint = [this](const std::string& s) { printed.push_back(s); };
        p.onStatus = [this](const std::string& s) { printed.push_back("status: " + s); };
        p.clock = [this] { return (double)frame / sr; };
        p.sampleRate = sr;
        p.dspRunning = true;
        p.ioBufferFrames = dsp::kBlock;
        if (!text.empty()) {
            if (!p.load(text, error)) std::printf("  rig: %s\n", error.c_str());
            p.loadbangAll();
        }
    }
    moritz::Box* box(const std::string& id) { return p.find(id); }
    // frames of output (interleaved, ch channels), in calls of `block` frames;
    // the timers due before each call run first
    std::vector<float> run(long frames, int block = dsp::kBlock) {
        std::vector<float> all, buf((size_t)block * ch);
        for (long done = 0; done < frames; done += block) {
            int n = (int)std::min<long>(block, frames - done);
            p.runDue((double)frame / sr);
            p.process(nullptr, 0, buf.data(), ch, n);
            all.insert(all.end(), buf.begin(), buf.begin() + (long)n * ch);
            frame += n;
        }
        return all;
    }
    bool printedSomething(const std::string& part) const {
        for (auto& s : printed)
            if (s.find(part) != std::string::npos) return true;
        return false;
    }
};

inline float peakOf(const std::vector<float>& x, size_t from = 0, size_t to = ~size_t(0)) {
    float p = 0.f;
    for (size_t i = from; i < std::min(to, x.size()); ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}

// one channel of interleaved output
inline std::vector<float> channel(const std::vector<float>& x, int ch, int c) {
    std::vector<float> y;
    for (size_t i = (size_t)c; i < x.size(); i += (size_t)ch) y.push_back(x[i]);
    return y;
}

}  // namespace etest
