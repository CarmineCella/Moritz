// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// harmonize_tests.cpp — harmonize~ (Musil's Psola) in the engine: nothing allocated on the audio thread whatever
// the messages; every message, a new range or latency (a new engine swapped in) and the box's deletion while
// another thread runs the audio (under ThreadSanitizer: clean); the same output for any block size; the f0 out.

#include "rig.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

using namespace moritz;
using etest::Rig;

namespace {
// a voice-like source: a sawtooth at 196 Hz, so the harmonizer has periods to find
const char* kVoice =
    "(patch (obj ph saw~ 196) (obj rs *~ 0.3) (obj h harmonize~ 4)"
    " (obj d dac~ 1 2) (obj pr print f0)"
    " (connect ph 0 rs 0) (connect rs 0 h 0) (connect h 0 d 0) (connect h 1 d 1)"
    " (connect h 2 pr 0))";

Message words(const std::string& s) {
    Message m;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        std::string w = s.substr(i, j - i);
        char* end = nullptr;
        float f = std::strtof(w.c_str(), &end);
        if (!w.empty() && end && *end == 0) m.push_back(Atom(f)); else if (!w.empty()) m.push_back(Atom(w.c_str()));
        i = j + 1;
    }
    return m;
}
const char* kMessages[] = {
    "mode scale", "key D", "scale dorian", "intervals 2 4 -3", "mode midi-chord", "note 60 100", "note 64 90",
    "note 67 70", "note 64 0", "mode midi-root", "intervals 0 7 12", "note 55 100", "mode intervals",
    "intervals 4 7 12 -12", "voice 2 formant 1.2", "voice 1 pan -1", "voice 3 vibrato 20 5 0.5", "level 0.8",
    "window blackman", "window hamming", "humanize 10 20", "seed 3", "cycles 1", "cycles auto", "marks period",
    "marks snap", "unvoiced dry", "unvoiced mute", "threshold 0.2", "gate -40", "transpose -12", "transpose 0",
    "mix 0.5 1", "scale 0 2 4 7 9", "flush", "window hann"};
bool finite(const std::vector<float>& x) {
    for (float v : x)
        if (!std::isfinite(v) || std::fabs(v) > 100.f) return false;
    return true;
}
}  // namespace

TEST(harmonize_allocates_nothing_on_the_audio_thread) {
    if (!etest::allocGuardActive()) {
        std::printf("  (skipped: the sanitizer's runtime owns operator new)\n");
        return;
    }
    Rig r(kVoice);
    r.run(8192);
    long before = etest::audioAllocations();
    const int sizes[] = {64, 17, 100, 512, 1, 33};
    std::vector<float> all;
    for (size_t k = 0; k < sizeof(kMessages) / sizeof(kMessages[0]); ++k) {
        r.box("h")->message(0, words(kMessages[k]));
        auto out = r.run(2000, sizes[k % 6]);
        all.insert(all.end(), out.begin(), out.end());
    }
    long n = etest::audioAllocations() - before;
    if (n) std::printf("  %ld allocations or frees on the audio thread\n", n);
    CHECK(n == 0);
    CHECK(finite(all));
    CHECK(etest::peakOf(all) > 0.01f);
}

TEST(harmonize_messages_and_deletion_while_audio_runs) {
    Rig r(kVoice);
    std::atomic<bool> stop{false};
    std::atomic<long> blocks{0}, broken{0};
    std::thread audio([&] {
        std::vector<float> buf(2 * 64);
        while (!stop) {
            r.p.process(nullptr, 0, buf.data(), 2, 64);
            for (float v : buf) if (!std::isfinite(v)) { broken.fetch_add(1); break; }
            blocks.fetch_add(1);
        }
    });
    for (int k = 0; k < 400; ++k) {
        r.box("h")->message(0, words(kMessages[k % (sizeof(kMessages) / sizeof(kMessages[0]))]));
        if (k % 40 == 7) r.box("h")->message(0, words(k % 80 == 7 ? "range 90 800" : "latency 25"));   // a new engine
        r.box("h")->message(2, {Atom((float)(48 + k % 24)), Atom((float)(k % 3 ? 90 : 0))});
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    // the box goes while the audio runs, and another comes
    r.p.removeBoxes({r.box("h")});
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::string err;
    Box* b = r.p.addBox(r.p.create("harmonize~", {Atom(2.f), Atom("separate")}), 200, 200);
    r.p.connect(r.box("rs"), 0, b, 0);
    r.p.connect(b, 0, r.box("d"), 0);
    r.p.connect(b, 1, r.box("d"), 1);
    b->message(0, words("mode scale"));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    r.p.removeBoxes({b});
    stop = true;
    audio.join();
    CHECK(blocks.load() > 0);
    CHECK(broken.load() == 0);
}

// the engine runs a sample at a time with its notes at their sample: the block size changes nothing
TEST(harmonize_same_output_for_any_block_size) {
    std::vector<float> ref;
    for (int block : {64, 17, 256}) {
        Rig r(kVoice);
        r.box("h")->message(0, words("mode scale"));
        r.box("h")->message(0, words("intervals 2 4 -3"));
        r.box("h")->message(0, words("humanize 8 20"));
        r.box("h")->message(0, words("vibrato 15 5 0.4"));
        auto out = r.run(44100, block);
        if (ref.empty()) { ref = out; continue; }
        float worst = 0.f;
        for (size_t i = 0; i < std::min(ref.size(), out.size()); ++i) worst = std::max(worst, std::fabs(ref[i] - out[i]));
        if (worst > 1e-6f) std::printf("  block %d: differs by %g\n", block, (double)worst);
        CHECK(worst <= 1e-6f);
    }
    CHECK(etest::peakOf(ref) > 0.01f);
}

// the f0 outlet: the source's 196 Hz, sent from the audio thread's value by a timer
TEST(harmonize_sends_the_f0) {
    Rig r(kVoice);
    r.run(44100);
    bool near196 = false;
    for (auto& s : r.printed) {
        float f = 0.f;
        if (std::sscanf(s.c_str(), "f0: %f", &f) == 1 && std::fabs(f - 196.f) < 2.f) near196 = true;
    }
    CHECK(near196);
}
