// live_rt.cpp — the live engine's audio thread, driven by hand: it neither allocates nor frees (every allocation
// and free during render is counted, with synths that use every kind of node, moving sources, voices that come
// and go, synths freed), a timed set-param lands on its very sample, and a voice's glide takes the same time
// whatever the block size.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause

#include "musil.h"

#include <cstdlib>
#include <new>

static thread_local bool counting = false;
static long allocations = 0, frees = 0;
void* operator new(std::size_t n) { if (counting) ++allocations; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](std::size_t n) { if (counting) ++allocations; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { if (p && counting) ++frees; std::free(p); }
void operator delete[](void* p) noexcept { if (p && counting) ++frees; std::free(p); }
void operator delete(void* p, std::size_t) noexcept { if (p && counting) ++frees; std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { if (p && counting) ++frees; std::free(p); }

using namespace musil;

static int failures = 0;
static void expect(bool ok, const std::string& what) { if (!ok) { std::cerr << "FAIL " << what << "\n"; ++failures; } }

int main() {
    Interp I; make_env(I);
    std::ostringstream out; I.out = &out;
    try { I.run("load \"live.mu\"\n", "<rt>"); } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
    audio_engine& e = engine();
    // a device that is never started: this test is the audio thread
    e.sr = 44100; e.block = 64; e.channels = 2; e.clock = 0; e.open = true; e.forget();
    std::vector<float> buf(4096 * 2);
    auto render = [&](int frames) { counting = true; e.render(buf.data(), frames); counting = false; };
    try {
        I.run("function src (freq gate) (* (osc 44100 freq saw-table) (adsr 44100 gate 0.01 0.1 0.5 0.2))\n"
              "function fx (x cut) (comb (allpass (delay (lowpass x 44100 cut 0.7) 100) 30 0.5) 40 0.3)\n"
              "function mix (y cut) (+ (svf y 44100 cut 1 \"low\") (* 0.01 (noise 1)) (conv y (vec 1 0.5 0.25)))\n"
              "function spatial (z az el) (+ (binaural z az el 44100) (ambi-decode (ambi-rotate (ambi-encode z 3 az el) az) (speaker-ring 2)) (pan-azimuth (tanh z) az))\n"
              "function rich (freq az el cut gate) (spatial (mix (fx (src freq gate) cut) cut) az el)\n"
              "var s1 (synth rich)\nvar s2 (synth rich)\n"
              "set-params s1 (list (list 'freq 220) (list 'cut 2000) (list 'gate 1))\n"
              "set-params s2 (list (list 'freq 330) (list 'cut 900) (list 'gate 1))\n"
              "var dc (+ (zeros 2000) 0.1)\n", "<rt>");
    } catch (const std::exception& ex) { std::cerr << ex.what() << "\n"; return 1; }
    render(64);
    allocations = frees = 0;
    for (int b = 0; b < 200; b++) {
        // the interpreter's side, between blocks: sources move, voices start and stop, synths come and go
        std::string az = std::to_string((b * 7) % 360 - 180), el = std::to_string((b * 3) % 80 - 40);
        I.run("set-param s1 'az " + az + "\nset-param s1 'el " + el + "\nset-param s2 'az " + az + " 0.01\n", "<rt>");
        if (b % 10 == 0) I.run("var v (play-buffer dc 44100 0.5 0 1 0 0)\n", "<rt>");
        if (b % 10 == 5) I.run("stop v\n", "<rt>");
        if (b == 100) I.run("free s2\nvar s3 (synth rich)\nset s2 s3\nset-param s3 'gate 1 0 0.05\n", "<rt>");
        render(b % 3 == 0 ? 64 : b % 3 == 1 ? 37 : 200);   // blocks of any size: the callback cuts them to the prepared size
    }
    expect(allocations == 0, "render allocates nothing (" + std::to_string(allocations) + " allocations)");
    expect(frees == 0, "render frees nothing (" + std::to_string(frees) + " frees): what it lets go of is freed elsewhere");
    e.retired.drain();
    I.run("free s1\nfree s3\nstop-all\n", "<rt>"); render(64); e.retired.drain();

    // a timed set-param lands on its sample, inside the block
    e.forget(); e.clock = 0; e.limiter = false;
    I.run("function level (x) x\nvar sl (synth level)\n", "<rt>");
    render(64);                                        // the synth is in
    long long when = 64 + 64 * 3 + 17;                 // sample 17 of the fourth block from here
    I.run("set-param sl 'x 0.25 0 " + std::to_string((double)when / 44100.0) + "\n", "<rt>");
    long long first = -1;
    for (int b = 0; b < 8; b++) { long long t0 = e.clock.load(); render(64); for (int k = 0; k < 64 && first < 0; k++) if (buf[(size_t)k * 2] == 0.25f) first = t0 + k; }
    expect(first == when, "set-param at a time: the change lands on its very sample (" + std::to_string(first) + ", wanted " + std::to_string(when) + ")");
    I.run("free sl\n", "<rt>"); render(64); e.retired.drain();

    // voice-set glides with the same time constant whatever the block size
    auto glide = [&](int block) {
        e.forget(); e.clock = 0; e.block = block;
        I.run("var g (play-buffer (+ (zeros 88200) 1) 44100 1 0 1 0 0)\n", "<rt>");
        render(block);
        double start = buf[0];
        I.run("voice-set g \"amp\" 0\n", "<rt>");
        long long t0 = e.clock.load();
        for (int b = 0; b < 4096 / block + 1; b++) {
            long long tb = e.clock.load(); render(block);
            for (int k = 0; k < block; k++) if (buf[(size_t)k * 2] < start * 0.36787944) return (double)(tb + k - t0);
        }
        return -1.0;
    };
    double g64 = glide(64), g512 = glide(512);
    expect(g64 > 0 && std::fabs(g64 - g512) <= 1, "voice-set glides in the same time with blocks of 64 and 512 (" + std::to_string(g64) + " and " + std::to_string(g512) + " samples)");
    expect(std::fabs(g64 - voice_glide * 44100) < 4, "...its time constant is voice_glide (" + std::to_string(g64) + " samples)");
    // more timed changes in one block than a parameter queues: the latest in time is the one that stays
    {
        param_node pn("p", 0.0);
        for (int k = 0; k < param_node::MAX_CHANGES + 8; ++k) pn.at(k, (double)k, 0);
        std::vector<std::unique_ptr<gnode>> none;
        pn.process(none, 64);
        expect(pn.target.load() == (double)(param_node::MAX_CHANGES + 7), "set-param: past the queue, the latest change is kept (" + std::to_string(pn.target.load()) + ")");
    }
    e.forget(); e.open = false;
    if (failures == 0) std::cout << "live_rt: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
