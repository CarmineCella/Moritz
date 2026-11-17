// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// hrtf_kemar.h — the MIT KEMAR head-related impulse responses (compact set,
// 128 taps at 44100 Hz, Gardner and Martin, MIT Media Laboratory 1994): every
// measured direction, sorted by elevation then azimuth. The data is Musil's,
// musil/src/hrtf_kemar.csv, read once when hrtf~, binaural~ or the binaural
// monitor first needs it.
#pragma once

#include <vector>

namespace moritz {
namespace kemar {

constexpr int kTaps = 128;
constexpr double kRate = 44100.0;

struct Direction {
    float az, el;              // degrees: azimuth positive to the left, elevation up
    float left[kTaps], right[kTaps];
};

// every direction (710), or none when the file is missing (the objects are then silent, and say so once)
const std::vector<Direction>& directions();

}  // namespace kemar
}  // namespace moritz
