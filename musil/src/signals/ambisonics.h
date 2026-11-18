// ambisonics.h — the spherical harmonics and the decoders of Musil's ambisonics (ambi-gains,
// ambi-decode, binaural-decode and their live nodes) and of Moritz's ambienc~, ambidec~ and binaural~.
// Self-contained (the standard library only).
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// Conventions (AmbiX): azimuth in degrees, 0 in front, positive to the LEFT; elevation in degrees,
// positive up. A sound field of order N has (N+1)^2 channels, in ACN order with SN3D normalisation:
// for order 1, W Y Z X.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace musil {
namespace dsp {

inline double deg2rad(double d) { return d * 3.14159265358979323846 / 180.0; }
// the real spherical harmonics up to `order` at (az, el): (order+1)^2 gains, ACN order, SN3D
inline std::vector<double> sh_gains(int order, double az, double el) {
    double a = deg2rad(az), e = deg2rad(el); double x = std::cos(e) * std::cos(a), y = std::cos(e) * std::sin(a), z = std::sin(e);
    std::vector<double> g((size_t)(order + 1) * (order + 1));
    // associated Legendre P_n^m(z) without Condon-Shortley phase, then SN3D: sqrt((2-d_m0) (n-|m|)!/(n+|m|)!)
    auto fact = [](int n) { double f = 1; for (int k = 2; k <= n; k++) f *= k; return f; };
    double r = std::sqrt(x * x + y * y);          // cos(elevation)
    for (int n = 0; n <= order; n++) for (int m = -n; m <= n; m++) {
        int am = std::abs(m);
        // P_n^|m|(z) by recurrence
        double pmm = 1; for (int k = 1; k <= am; k++) pmm *= (2 * k - 1) * r;      // P_m^m = (2m-1)!! (1-z^2)^(m/2)
        double p = pmm;
        if (n == am) p = pmm;
        else { double p1 = z * (2 * am + 1) * pmm; if (n == am + 1) p = p1; else { double pk2 = pmm, pk1 = p1; for (int k = am + 2; k <= n; k++) { p = ((2 * k - 1) * z * pk1 - (k + am - 1) * pk2) / (k - am); pk2 = pk1; pk1 = p; } } }
        double norm = std::sqrt((m == 0 ? 1.0 : 2.0) * fact(n - am) / fact(n + am));
        double azf = m > 0 ? std::cos(m * a) : m < 0 ? std::sin(am * a) : 1.0;
        g[(size_t)(n * n + n + m)] = norm * p * azf;
    }
    return g;
}
// The decoding matrix of ambi-decode (of its streaming node in live.h, of ambidec~ and binaural~): one row of gains per speaker.
// Speakers all at ear height form a ring, which hears only the horizontal (sectoral) channels: the decoder is
// the exact mode-matching one for an even ring of at least 2N+1 speakers (each order scaled by 2 over the
// speaker count and the SN3D factor of its cos term), with 2D max-rE weights cos(n pi / (2N + 2)); the energy
// vector of a source then reaches its theoretical 0.71 at order 1 and 0.87 at order 2. Speakers with elevations
// sample the sphere: every channel of order n scaled by 2n+1 (SN3D to a sampling decoder) and weighted by the
// 3D max-rE weights P_n(rE), rE = cos(137.9 degrees / (N + 1.51)), all over the speaker count.
inline std::vector<std::vector<double>> ambi_decoder(int order, const std::vector<std::pair<double, double>>& dirs) {
    const double pi = 3.14159265358979323846; size_t nch = (size_t)(order + 1) * (order + 1); double ns = (double)std::max<size_t>(1, dirs.size());
    bool ring = true; for (auto& d : dirs) if (std::fabs(d.second) > 1e-9) ring = false;
    std::vector<std::vector<double>> M;
    if (ring) {
        auto front = sh_gains(order, 0, 0);
        for (auto& d : dirs) {
            std::vector<double> row(nch, 0.0); double a = deg2rad(d.first);
            row[0] = 1.0 / ns;
            for (int n = 1; n <= order; n++) {
                double wn = std::cos(n * pi / (2 * order + 2)), c = front[(size_t)(n * n + 2 * n)], scale = 2.0 * wn / (ns * c);
                row[(size_t)(n * n + 2 * n)] = scale * std::cos(n * a);       // m = +n
                row[(size_t)(n * n)] = scale * std::sin(n * a);               // m = -n
            }
            M.push_back(std::move(row));
        }
        return M;
    }
    double rE = std::cos(137.9 * pi / 180.0 / (order + 1.51)); std::vector<double> w((size_t)order + 1);
    double p0 = 1, p1 = rE;
    for (int n = 0; n <= order; n++) { double pn = n == 0 ? 1.0 : n == 1 ? rE : ((2 * n - 1) * rE * p1 - (n - 1) * p0) / n; if (n >= 2) { p0 = p1; p1 = pn; } w[(size_t)n] = pn * (2 * n + 1); }
    for (auto& d : dirs) {
        auto g = sh_gains(order, d.first, d.second); std::vector<double> row(nch);
        for (int n = 0; n <= order; n++) for (int m = -n; m <= n; m++) { size_t k = (size_t)(n * n + n + m); row[k] = g[k] * w[(size_t)n] / ns; }
        M.push_back(std::move(row));
    }
    return M;
}
}  // namespace dsp
}  // namespace musil
