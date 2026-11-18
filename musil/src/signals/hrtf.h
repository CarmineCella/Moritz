// hrtf.h — reads a set of head-related impulse responses from a CSV (as musil/src/hrtf_kemar.csv, the
// MIT KEMAR compact set at 44100 Hz): one line per direction, azimuth and elevation in degrees, then
// the left ear's samples and the right's (as many of each); lines starting with # are comments.
// Musil's hrtf-load and Moritz's hrtf~ and binaural~ read their sets with it. Self-contained.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <cstdlib>
#include <istream>
#include <string>
#include <vector>

namespace musil {
namespace dsp {

struct HrtfDirection {
    double az = 0, el = 0;                   // azimuth positive to the left, elevation up
    std::vector<double> left, right;
};

// the directions of the set in `in`, in file order; lines that are not az, el and two equal halves are skipped
inline std::vector<HrtfDirection> read_hrtf_csv(std::istream& in) {
    std::vector<HrtfDirection> out;
    std::string line;
    std::vector<double> v;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        v.clear();
        const char* p = line.c_str();
        char* end = nullptr;
        for (;;) {
            double x = std::strtod(p, &end);
            if (end == p) break;
            v.push_back(x);
            p = end;
            while (*p == ',' || *p == ' ' || *p == '\t') ++p;
        }
        if (v.size() < 4 || (v.size() - 2) % 2) continue;
        size_t n = (v.size() - 2) / 2;
        HrtfDirection d;
        d.az = v[0];
        d.el = v[1];
        d.left.assign(v.begin() + 2, v.begin() + 2 + (long)n);
        d.right.assign(v.begin() + 2 + (long)n, v.end());
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace dsp
}  // namespace musil
