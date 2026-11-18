// engines.cpp — the C++ engines of Musil's libraries (src/signals/*.h, src/scientific/*.h, src/music/*.h), tested
// from C++ as a host such as Moritz uses them: resampling, the random numbers, the convolution engines,
// the vocoder, the decoders, the MIDI reader, Orchidea's search; and the shared state of the libraries used by
// interpreters on several threads at once.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause

#include "musil.h"

#include <chrono>
#include <complex>
#include <cstdio>
#include <thread>

using namespace musil;

static int failures = 0;
static void expect(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAIL " << what << "\n"; ++failures; }
}


// --- the libraries' shared state under interpreters on threads (as in Moritz) ---
static void test_threads() {
    // resample's kernel table: two threads, two factors, the same samples as on one thread
    varr x(0.0, 3000);
    for (size_t k = 0; k < x.size(); ++k) x[k] = std::sin(0.01 * (double)k * (double)k / 300.0);
    varr r1 = resample_sinc(x, 0.37), r2 = resample_sinc(x, 0.61);
    std::atomic<int> bad{0};
    auto same = [](const varr& a, const varr& b) { if (a.size() != b.size()) return false; for (size_t k = 0; k < a.size(); ++k) if (a[k] != b[k]) return false; return true; };
    {
        std::thread t1([&] { for (int k = 0; k < 40; ++k) if (!same(resample_sinc(x, k % 2 ? 0.37 : 0.61), k % 2 ? r1 : r2)) ++bad; });
        std::thread t2([&] { for (int k = 0; k < 40; ++k) if (!same(resample_sinc(x, k % 2 ? 0.61 : 0.37), k % 2 ? r2 : r1)) ++bad; });
        t1.join(); t2.join();
    }
    expect(bad == 0, "resample on two threads at once gives the samples of one thread");
    // the head responses: one interpreter loads and unloads the set while another asks for responses
    std::atomic<int> errors{0};
    auto run = [&](const std::string& code) {
        Interp I; make_env(I); std::ostringstream out; I.out = &out;
        try { I.run(code, "<threads>"); } catch (const std::exception& e) { std::cerr << e.what() << "\n"; ++errors; }
        return out.str();
    };
    {
        std::thread loader([&] { run("load \"signals.mu\"\ntimes 4 (function (k) { hrtf-load \"hrtf_kemar.csv\"\nhrtf-unload\n })\nhrtf-load \"hrtf_kemar.csv\"\n"); });
        std::thread user([&] {
            std::string o = run("load \"signals.mu\"\ntimes 200 (function (k) (if (< (length (head (hrir (* 7 k) 0 (if (even? k) 48000 22050)))) 8) (error \"short\")))\nprint \"ok\"\n");
            if (o != "ok\n") ++errors;
        });
        loader.join(); user.join();
    }
    expect(errors == 0, "hrir while another interpreter loads and unloads the set");
    // the index of a sample folder: built once, read by two interpreters
    std::string a, b;
    {
        std::thread t1([&] { a = run("load \"music.mu\"\nprint (db-index-size \"../../data\")\n"); });
        std::thread t2([&] { b = run("load \"music.mu\"\nprint (db-index-size \"../../data\")\n"); });
        t1.join(); t2.join();
    }
    expect(!a.empty() && a == b, "db-index-size on two threads");
}

int main() {
    test_threads();
    if (failures == 0) std::cout << "engines: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
