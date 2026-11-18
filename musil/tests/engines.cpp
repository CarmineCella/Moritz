// engines.cpp — the C++ engines of Musil's libraries (src/signals/*.h, src/scientific/*.h, src/music/*.h), tested
// from C++ as a host such as Moritz uses them: the FFTs, resampling, the random numbers, the convolution engines,
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


// --- the FFTs: FFT<T> against a direct DFT, RealFFT<T> against FFT<T>, both ways ---
static void test_fft() {
    for (int n : {1, 2, 4, 8, 64, 1024}) {
        std::vector<double> x((size_t)n);
        for (int k = 0; k < n; ++k) x[(size_t)k] = std::sin(0.37 * k * k + 1.0) + 0.25 * std::cos(3.1 * k);
        std::vector<double> re(x), im((size_t)n, 0.0);
        dsp::FFT<double> f; f.init(n); f.forward(re.data(), im.data());
        double err = 0;
        for (int k = 0; k < n; ++k) {
            std::complex<double> s = 0;
            for (int t = 0; t < n; ++t) s += x[(size_t)t] * std::polar(1.0, -dsp::kTwoPi * k * t / n);
            err = std::max(err, std::abs(s - std::complex<double>(re[(size_t)k], im[(size_t)k])));
        }
        expect(err < 1e-9 * n, "FFT<double> matches the DFT at n = " + std::to_string(n));
        if (n < 2) continue;
        dsp::RealFFT<double> rf; rf.init(n);
        std::vector<double> rr((size_t)n / 2 + 1), ri((size_t)n / 2 + 1), back((size_t)n);
        rf.forward(x.data(), rr.data(), ri.data());
        double e2 = 0;
        for (int k = 0; k <= n / 2; ++k) e2 = std::max(e2, std::hypot(rr[(size_t)k] - re[(size_t)k], ri[(size_t)k] - im[(size_t)k]));
        expect(e2 < 1e-10 * n, "RealFFT<double> forward matches FFT<double> at n = " + std::to_string(n));
        rf.inverse(rr.data(), ri.data(), back.data());
        double e3 = 0;
        for (int k = 0; k < n; ++k) e3 = std::max(e3, std::fabs(back[(size_t)k] / n - x[(size_t)k]));
        expect(e3 < 1e-12 * n, "RealFFT<double> inverse(forward(x)) = n x at n = " + std::to_string(n));
    }
    // float, as the real-time objects use it
    dsp::RealFFT<float> rf; rf.init(512);
    std::vector<float> x(512), re(257), im(257), back(512);
    for (int k = 0; k < 512; ++k) x[(size_t)k] = (float)std::sin(dsp::kTwoPi * 10 * k / 512);
    rf.forward(x.data(), re.data(), im.data());
    expect(std::fabs(std::hypot(re[10], im[10]) - 256) < 1e-2, "RealFFT<float>: a sine at bin 10");
    rf.inverse(re.data(), im.data(), back.data());
    float e = 0; for (int k = 0; k < 512; ++k) e = std::max(e, std::fabs(back[(size_t)k] / 512 - x[(size_t)k]));
    expect(e < 1e-5f, "RealFFT<float> round trip");
}

// --- resampling: the anti-aliasing of a downsampling, the passband ---
static double level_of(const varr& y, double f, double sr) {          // the amplitude of a sine of f in y (least squares)
    double c = 0, s = 0, cc = 0, ss = 0;
    size_t a = y.size() / 4, b = 3 * y.size() / 4;                      // away from the edges
    for (size_t k = a; k < b; ++k) { double w = dsp::kTwoPi * f * (double)k / sr; c += y[k] * std::cos(w); s += y[k] * std::sin(w); cc += std::cos(w) * std::cos(w); ss += std::sin(w) * std::sin(w); }
    return std::hypot(c / cc, s / ss);
}
static void test_resample() {
    const double sr = 44100, factor = 0.1, nsr = sr * factor;          // to 4410 Hz: the new Nyquist is 2205 Hz
    auto tone = [&](double f) { varr x(0.0, (size_t)sr); for (size_t k = 0; k < x.size(); ++k) x[k] = std::sin(dsp::kTwoPi * f * (double)k / sr); return x; };
    double pass = level_of(resample_sinc(tone(0.8 * 2205), factor), 0.8 * 2205, nsr);
    expect(std::fabs(20 * std::log10(pass)) < 0.2, "resample 0.1: 0.8 of the new Nyquist passes (" + std::to_string(20 * std::log10(pass)) + " dB)");
    // 4340 Hz would alias to 70 Hz at 4410 Hz
    double alias = level_of(resample_sinc(tone(4410 - 70), factor), 70, nsr);
    expect(20 * std::log10(alias + 1e-30) < -60, "resample 0.1: what would alias is under -60 dB (" + std::to_string(20 * std::log10(alias + 1e-30)) + " dB)");
    varr dc(1.0, 1000), y = resample_sinc(dc, 0.37);
    expect(std::fabs(y[y.size() / 2] - 1) < 1e-9, "resample: a constant stays a constant");
}

// --- the MIDI reader: overlapping notes of a pitch, truncated and corrupt files ---
static void test_midi() {
    std::vector<unsigned char> head = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0};   // format 0, 1 track, 480 ppq
    std::vector<unsigned char> trk = {0x00, 0x90, 60, 100,          // on at 0
                                      0x81, 0x70, 0x90, 60, 80,     // on again at 240, before the first ends
                                      0x81, 0x70, 0x80, 60, 0,      // off at 480: ends the first
                                      0x81, 0x70, 0x80, 60, 0,      // off at 720: ends the second
                                      0x00, 0xff, 0x2f, 0x00};
    std::vector<unsigned char> f = head;
    for (unsigned char c : {'M', 'T', 'r', 'k'}) f.push_back(c);
    for (int s = 24; s >= 0; s -= 8) f.push_back((unsigned char)((trk.size() >> s) & 0xff));
    f.insert(f.end(), trk.begin(), trk.end());
    midi_file m = parse_midi(f, "overlap");
    expect(m.notes.size() == 2, "midi: a pitch struck again before its note-off makes two notes");
    if (m.notes.size() == 2) {
        expect(std::fabs(m.notes[0].at) < 1e-9 && std::fabs(m.notes[0].dur - 0.5) < 1e-9 && m.notes[0].velocity == 100, "midi: the first note ends at the first note-off");
        expect(std::fabs(m.notes[1].at - 0.25) < 1e-9 && std::fabs(m.notes[1].dur - 0.5) < 1e-9 && m.notes[1].velocity == 80, "midi: the second at the second");
    }
    // every truncation and many corruptions of a real file: read up to the damage, never past the data
    std::ifstream in("../../data/test.mid", std::ios::binary);
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    expect(d.size() > 14, "midi: the test file");
    size_t read = 0;
    for (size_t n = 0; n <= d.size(); ++n) {
        std::vector<unsigned char> part(d.begin(), d.begin() + (long)n);
        try { midi_file t = parse_midi(part, "truncated"); read += t.notes.size(); } catch (std::exception&) {}
    }
    expect(read > 0, "midi: truncated files give the notes before the cut");
    std::mt19937 g(7);
    for (int k = 0; k < 2000; ++k) {
        std::vector<unsigned char> bad = d;
        for (int j = 0; j < 4; ++j) bad[14 + rnd::below(g, (uint32_t)(bad.size() - 14))] = (unsigned char)rnd::below(g, 256);
        bad.resize(14 + rnd::below(g, (uint32_t)(bad.size() - 13)));
        try { parse_midi(bad, "corrupt"); } catch (std::exception&) {}
    }
}

// --- Orchidea's genetic search keeps the best it has found (elitism) ---
static void test_orchidea() {
    std::mt19937 g(11);
    std::vector<orchidea::entry> db;
    const char* instr[3] = {"A", "B", "C"};
    for (int k = 0; k < 60; ++k) {
        orchidea::entry e; e.index = k; e.instr = instr[k % 3]; e.pitch = "C4"; e.midi = 60; e.tech = "ord"; e.dyn = "mf";
        for (int j = 0; j < 24; ++j) e.features.push_back((float)rnd::uniform01(g));
        db.push_back(e);
    }
    orchidea::segment seg; seg.length = 1;
    seg.features.assign(24, 0.f);
    for (int k : {4, 20, 41}) for (int j = 0; j < 24; ++j) seg.features[(size_t)j] += db[(size_t)k].features[(size_t)j] / 3;
    orchidea::standardise(seg.features);
    for (unsigned seed = 1; seed <= 4; ++seed) {
        orchidea::model m;
        orchidea::make_model(db, {"A", "B", "C"}, seg, {}, {}, {}, m);
        orchidea::params p; p.pop_size = 24; p.max_epochs = 40; p.mutation_rate = 0.4; p.sparsity = 0.1; p.seed = seed;
        // the first population, drawn as the search draws it, evaluated
        orchidea::genetic first(p);
        std::vector<orchidea::solution> pop((size_t)p.pop_size);
        for (auto& id : pop) first.random_chromosome(m, id.indices);
        std::vector<float> values(seg.features.size());
        first.evaluate_population(pop, seg.features, m.database, values);
        float best0 = 0; for (auto& s : pop) best0 = std::max(best0, s.fitness);
        orchidea::genetic search(p);
        search.search(m);
        expect(!m.solutions.empty() && m.solutions[0].fitness >= best0, "orchidea: the best solution is never lost (seed " + std::to_string(seed) + ")");
    }
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

// --- the flush-to-zero guard sets the mode for its scope and gives the caller's back ---
static void test_denormals() {
    bool before = dsp::DenormalGuard::active();
    {
        dsp::DenormalGuard g;
#if defined(MUSIL_FTZ_SSE) || defined(MUSIL_FTZ_ARM64)
        expect(dsp::DenormalGuard::active(), "denormals: flushed inside the guard");
        volatile double tiny = 1e-300, f = 1e-10;
        expect(tiny * f == 0.0, "denormals: a denormal result is 0 inside the guard");
#endif
    }
    expect(dsp::DenormalGuard::active() == before, "denormals: the caller's mode is restored");
}

int main() {
    test_denormals();
    test_threads();
    test_fft();
    test_resample();
    test_midi();
    test_orchidea();
    if (failures == 0) std::cout << "engines: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
