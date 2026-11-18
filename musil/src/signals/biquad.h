// biquad.h — the coefficients of the RBJ biquads (Robert Bristow-Johnson's Audio EQ Cookbook): Musil's biquad
// (and the filters built on it: lowpass, highpass, bandpass, notch, peak-eq, lowshelf, highshelf), the loudness
// weighting of mir.h, and Moritz's biquad objects. Self-contained (the standard library only).
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// The transfer function is (b0 + b1 z^-1 + b2 z^-2) / (a0 + a1 z^-1 + a2 z^-2); rbj() gives the cookbook's
// coefficients as they are (a0 is not 1), normalized() divides them all by a0. f0 in Hz, q the quality factor
// (0.7071 for a Butterworth low or high pass), gain in dB for peak and the shelves (the others ignore it).
#pragma once

#include <cmath>
#include <string>

namespace musil {
namespace dsp {

enum class BiquadType { Lowpass, Highpass, Bandpass, Notch, Peak, Lowshelf, Highshelf, Allpass };

struct BiquadCoeffs {
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    BiquadCoeffs normalized() const { return {b0 / a0, b1 / a0, b2 / a0, 1.0, a1 / a0, a2 / a0}; }
};

// "lowpass" "highpass" "bandpass" (constant 0 dB peak gain) "notch" "peak" "lowshelf" "highshelf" "allpass"
inline bool biquad_type(const std::string& name, BiquadType& t) {
    static const struct { const char* n; BiquadType t; } names[] = {
        {"lowpass", BiquadType::Lowpass}, {"highpass", BiquadType::Highpass}, {"bandpass", BiquadType::Bandpass},
        {"notch", BiquadType::Notch}, {"peak", BiquadType::Peak}, {"lowshelf", BiquadType::Lowshelf}, {"highshelf", BiquadType::Highshelf}, {"allpass", BiquadType::Allpass}};
    for (auto& e : names) if (name == e.n) { t = e.t; return true; }
    return false;
}

inline BiquadCoeffs rbj(BiquadType type, double sr, double f0, double q, double gainDb = 0) {
    const double w0 = 2 * 3.14159265358979323846 * f0 / sr, c = std::cos(w0), s = std::sin(w0), alpha = s / (2 * q);
    const double A = std::pow(10.0, gainDb / 40), sA = std::sqrt(A);
    switch (type) {
        case BiquadType::Lowpass:  return {(1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha};
        case BiquadType::Highpass: return {(1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha};
        case BiquadType::Bandpass: return {alpha, 0, -alpha, 1 + alpha, -2 * c, 1 - alpha};
        case BiquadType::Notch:    return {1, -2 * c, 1, 1 + alpha, -2 * c, 1 - alpha};
        case BiquadType::Allpass:  return {1 - alpha, -2 * c, 1 + alpha, 1 + alpha, -2 * c, 1 - alpha};
        case BiquadType::Peak:     return {1 + alpha * A, -2 * c, 1 - alpha * A, 1 + alpha / A, -2 * c, 1 - alpha / A};
        case BiquadType::Lowshelf:
            return {A * ((A + 1) - (A - 1) * c + 2 * sA * alpha), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - 2 * sA * alpha),
                    (A + 1) + (A - 1) * c + 2 * sA * alpha, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - 2 * sA * alpha};
        case BiquadType::Highshelf:
            return {A * ((A + 1) + (A - 1) * c + 2 * sA * alpha), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - 2 * sA * alpha),
                    (A + 1) - (A - 1) * c + 2 * sA * alpha, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - 2 * sA * alpha};
    }
    return {};
}

}  // namespace dsp
}  // namespace musil
