// hrtf_kemar.h — the MIT KEMAR head-related impulse responses (compact set,
// 128 taps at 44100 Hz, Gardner and Martin, MIT Media Laboratory 1994): every
// measured direction, sorted by elevation then azimuth. The data is in
// hrtf_kemar.cpp.
#pragma once

namespace moritz {
namespace kemar {

constexpr int kTaps = 128;
constexpr double kRate = 44100.0;
constexpr int kDirections = 710;

struct Direction {
    float az, el;              // degrees: azimuth positive to the left, elevation up
    float left[kTaps], right[kTaps];
};

extern const Direction kSet[kDirections];

}  // namespace kemar
}  // namespace moritz
