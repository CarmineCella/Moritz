// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// hrtf_kemar.cpp — reads the MIT KEMAR set from Musil's data (musil/src/hrtf_kemar.csv): one line per
// direction, azimuth and elevation then 128 samples of the left ear and 128 of the right; lines starting
// with # are comments. Data copyright 1994 MIT Media Laboratory (Bill Gardner and Keith Martin, "HRTF
// Measurements of a KEMAR Dummy-Head Microphone"), free to use provided the authors are cited.
#include "hrtf_kemar.h"
#include "gui.h"
#include "signals/hrtf.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace moritz {
namespace kemar {

namespace {

std::vector<Direction> load() {
    std::vector<Direction> out;
    std::string dir = resourceDir("musil");
    std::string path = dir.empty() ? "" : dir + "/src/hrtf_kemar.csv";
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "moritz: cannot read the KEMAR head responses (musil/src/hrtf_kemar.csv): hrtf~ and binaural~ are silent\n");
        return out;
    }
    for (auto& h : musil::dsp::read_hrtf_csv(f)) {      // Musil's reader (musil/src/signals/hrtf.h)
        if ((int)h.left.size() != kTaps) continue;
        Direction d{};
        d.az = (float)h.az;
        d.el = (float)h.el;
        for (int t = 0; t < kTaps; ++t) {
            d.left[t] = (float)h.left[(size_t)t];
            d.right[t] = (float)h.right[(size_t)t];
        }
        out.push_back(d);
    }
    return out;
}

}  // namespace

const std::vector<Direction>& directions() {
    static const std::vector<Direction> set = load();     // thread-safe, once
    return set;
}

}  // namespace kemar
}  // namespace moritz
