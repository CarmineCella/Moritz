// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// wav_tests.cpp — Moritz's one WAV writer: what it writes Musil's reader (and miniaudio) read back,
// and the file appears only when it is complete

#include "rig.h"
#include "system/wav_tools.h"

#include <cmath>
#include <cstdlib>
#include <fstream>

using namespace moritz;

namespace {
std::string temp(const std::string& name) {
    const char* t = std::getenv("TMPDIR");
    std::string d = t && *t ? t : "/tmp";
    if (d.back() != '/') d += '/';
    return d + name;
}
bool exists(const std::string& p) { return std::ifstream(p).good(); }
}  // namespace

TEST(wav_writer_round_trip) {
    for (int bits : {16, 24}) {
        std::string path = temp("moritz-engine-" + std::to_string(bits) + ".wav");
        std::remove(path.c_str());
        std::vector<float> x(2 * 1000);
        for (size_t i = 0; i < x.size(); ++i) x[i] = (float)std::sin(0.01 * (double)i) * 0.9f;
        x[10] = 2.f;                                     // clipped
        x[11] = std::nanf("");                           // silence
        WavWriter w;
        CHECK(w.open(path, 2, 48000, bits));
        w.write(x.data(), 600);
        w.write(x.data() + 1200, 400);
        CHECK(!exists(path));                            // not before it is complete
        CHECK(w.finish());
        CHECK(exists(path) && !exists(path + ".part"));
        WAVHeader h;
        auto ch = read_wav_raw(path.c_str(), h);         // Musil's reader
        CHECK(h.numChannels == 2 && h.sampleRate == 48000 && h.bitsPerSample == bits);
        CHECK(ch.size() == 2 && ch[0].size() == 1000);
        double tol = 2.0 / (double)(1L << (bits - 1));     // a step, and 32767 against 32768
        bool same = ch.size() == 2 && ch[0].size() == 1000;
        for (size_t f = 0; same && f < 1000; ++f)
            for (int c = 0; c < 2; ++c) {
                float want = x[f * 2 + (size_t)c];
                want = std::isfinite(want) ? std::clamp(want, -1.f, 1.f) : 0.f;
                same = same && std::fabs(ch[(size_t)c][f] - want) < tol;
            }
        CHECK(same);
        SampleBuffer b;                                  // and miniaudio, as buffer~ read does
        std::string err;
        CHECK(decodeAudioFile(path, b, err) && b.frames == 1000 && b.channels == 2);
        std::remove(path.c_str());
    }
    // abandoned: no file, no temporary left behind
    std::string path = temp("moritz-engine-abandoned.wav");
    {
        WavWriter w;
        w.open(path, 1, 44100, 16);
        float v = 0.5f;
        w.write(&v, 1);
    }
    CHECK(!exists(path) && !exists(path + ".part"));
}
