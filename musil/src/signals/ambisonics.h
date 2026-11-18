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
namespace ambi_detail {
// the eigenvalues (descending) and eigenvectors (columns of V) of a symmetric K x K matrix A (row-major), by Jacobi
inline void eig_sym(std::vector<double> A, int K, std::vector<double>& vals, std::vector<double>& V) {
    V.assign((size_t)K * K, 0.0);
    for (int k = 0; k < K; ++k) V[(size_t)(k * K + k)] = 1;
    double total = 0; for (double v : A) total += v * v;
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0;
        for (int p = 0; p < K; ++p) for (int q = p + 1; q < K; ++q) off += A[(size_t)(p * K + q)] * A[(size_t)(p * K + q)];
        if (off <= 1e-30 * total) break;
        for (int p = 0; p < K; ++p) for (int q = p + 1; q < K; ++q) {
            double apq = A[(size_t)(p * K + q)];
            if (std::fabs(apq) < 1e-300) continue;
            double theta = (A[(size_t)(q * K + q)] - A[(size_t)(p * K + p)]) / (2 * apq);
            double t = (theta >= 0 ? 1 : -1) / (std::fabs(theta) + std::sqrt(1 + theta * theta));
            double c = 1 / std::sqrt(1 + t * t), s = t * c;
            for (int k = 0; k < K; ++k) { double a = A[(size_t)(k * K + p)], b = A[(size_t)(k * K + q)]; A[(size_t)(k * K + p)] = c * a - s * b; A[(size_t)(k * K + q)] = s * a + c * b; }
            for (int k = 0; k < K; ++k) { double a = A[(size_t)(p * K + k)], b = A[(size_t)(q * K + k)]; A[(size_t)(p * K + k)] = c * a - s * b; A[(size_t)(q * K + k)] = s * a + c * b; }
            for (int k = 0; k < K; ++k) { double a = V[(size_t)(k * K + p)], b = V[(size_t)(k * K + q)]; V[(size_t)(k * K + p)] = c * a - s * b; V[(size_t)(k * K + q)] = s * a + c * b; }
        }
    }
    vals.resize((size_t)K);
    for (int k = 0; k < K; ++k) vals[(size_t)k] = A[(size_t)(k * K + k)];
}
// The energy-preserving decoder of the rows Y (L speakers x K harmonics, orthonormal harmonics: their mean square
// over the circle or the sphere is 1): D = Y (Y^T Y)^(-1/2), the pseudo-inverse square root on a rank-deficient
// layout (fewer speakers than harmonics); then every column k weighted by w[k] and the whole scaled so that the
// mean energy of a source over all directions is sum(w^2) / L, that of the exact decoder of a regular layout
inline std::vector<std::vector<double>> epad(const std::vector<std::vector<double>>& Y, const std::vector<double>& w) {
    size_t L = Y.size(), K = w.size();
    std::vector<double> G(K * K, 0.0);
    for (auto& row : Y) for (size_t a = 0; a < K; ++a) for (size_t b = 0; b < K; ++b) G[a * K + b] += row[a] * row[b];
    std::vector<double> vals, V;
    eig_sym(G, (int)K, vals, V);
    double top = 0; for (double v : vals) top = std::max(top, v);
    std::vector<double> M(K * K, 0.0);                          // (Y^T Y)^(-1/2) on the range
    for (size_t e = 0; e < K; ++e) {
        if (vals[e] <= 1e-9 * top) continue;
        double s = 1 / std::sqrt(vals[e]);
        for (size_t a = 0; a < K; ++a) for (size_t b = 0; b < K; ++b) M[a * K + b] += V[a * K + e] * s * V[b * K + e];
    }
    std::vector<std::vector<double>> D(L, std::vector<double>(K, 0.0));
    for (size_t l = 0; l < L; ++l) for (size_t b = 0; b < K; ++b) { double s = 0; for (size_t a = 0; a < K; ++a) s += Y[l][a] * M[a * K + b]; D[l][b] = s; }
    double want = 0, have = 0;                                   // the mean energy: sum_k w_k^2 (D^T D)_kk
    for (size_t k = 0; k < K; ++k) { double dd = 0; for (size_t l = 0; l < L; ++l) dd += D[l][k] * D[l][k]; want += w[k] * w[k]; have += w[k] * w[k] * dd; }
    double c = have > 0 ? std::sqrt(want / ((double)std::max<size_t>(1, L) * have)) : 0.0;
    for (auto& row : D) for (size_t k = 0; k < K; ++k) row[k] *= c * w[k];
    return D;
}
// is a ring of azimuths (degrees) evenly spaced, whatever its rotation and order?
inline bool regular_ring(const std::vector<std::pair<double, double>>& dirs) {
    size_t L = dirs.size(); if (L < 2) return L == 1;
    std::vector<double> a; for (auto& d : dirs) { double v = std::fmod(d.first, 360.0); if (v < 0) v += 360; a.push_back(v); }
    std::sort(a.begin(), a.end());
    double step = 360.0 / (double)L;
    for (size_t k = 0; k < L; ++k) { double gap = (k + 1 < L ? a[k + 1] : a[0] + 360) - a[k]; if (std::fabs(gap - step) > 1e-6) return false; }
    return true;
}
}  // namespace ambi_detail

// The decoding matrix of ambi-decode (of its streaming node in live.h, of ambidec~ and binaural~): one row of gains
// per speaker, (order+1)^2 columns (SN3D, ACN).
//   - Speakers all at ear height form a ring, which hears only the horizontal (sectoral) channels. An evenly spaced
//     ring of at least 2N+1 speakers gets the exact mode-matching decoder (each order scaled by 2 over the speaker
//     count and the SN3D factor of its cos term). Any other ring (irregular, as 5.0, or with fewer speakers than
//     2N+1) gets the energy-preserving decoder (EPAD, Zotter, Pomberger and Noisternig 2012) of its circular
//     harmonics: D = Y (Y^T Y)^(-1/2), so a source keeps its loudness whatever its direction (the plain formula on
//     5.0 made the front 5.4 dB louder than the back). Both with the 2D max-rE weights cos(n pi / (2N + 2)): the
//     energy vector of a source on a regular ring reaches 0.71 at order 1 and 0.87 at order 2.
//   - Speakers with elevations: the energy-preserving decoder of the spherical harmonics (N3D inside), with the 3D
//     max-rE weights P_n(rE), rE = cos(137.9 degrees / (N + 1.51)). On a t-design it is the sampling decoder; on any
//     other layout (binaural-decode's 17 directions) it is what the sampling decoder only approximates.
//   The scale: the mean energy of a source over all directions is the sum of the squared weights over the speaker
//   count, as for the exact decoder of a regular layout, so a layout and its regular counterpart play alike.
inline std::vector<std::vector<double>> ambi_decoder(int order, const std::vector<std::pair<double, double>>& dirs) {
    const double pi = 3.14159265358979323846; size_t nch = (size_t)(order + 1) * (order + 1); double ns = (double)std::max<size_t>(1, dirs.size());
    bool ring = true; for (auto& d : dirs) if (std::fabs(d.second) > 1e-9) ring = false;
    std::vector<std::vector<double>> M;
    if (dirs.empty()) return M;
    auto front = sh_gains(order, 0, 0);
    if (ring && ambi_detail::regular_ring(dirs) && dirs.size() >= (size_t)(2 * order + 1)) {
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
    if (ring) {
        // circular harmonics 1, sqrt2 cos n a, sqrt2 sin n a (orthonormal over the circle); the SN3D sectoral channel
        // (n, +-n) of a source is front_n times cos / sin n a
        size_t K = (size_t)(2 * order + 1);
        std::vector<std::vector<double>> Y;
        for (auto& d : dirs) {
            std::vector<double> y(K); double a = deg2rad(d.first); y[0] = 1;
            for (int n = 1; n <= order; n++) { y[(size_t)(2 * n - 1)] = std::sqrt(2.0) * std::cos(n * a); y[(size_t)(2 * n)] = std::sqrt(2.0) * std::sin(n * a); }
            Y.push_back(y);
        }
        std::vector<double> w(K, 1.0);
        for (int n = 1; n <= order; n++) w[(size_t)(2 * n - 1)] = w[(size_t)(2 * n)] = std::cos(n * pi / (2 * order + 2));
        auto D = ambi_detail::epad(Y, w);
        for (auto& r : D) {
            std::vector<double> row(nch, 0.0); row[0] = r[0];
            for (int n = 1; n <= order; n++) {
                double c = front[(size_t)(n * n + 2 * n)];           // the circular harmonic of a source: sqrt2 B / c
                row[(size_t)(n * n + 2 * n)] = r[(size_t)(2 * n - 1)] * std::sqrt(2.0) / c;
                row[(size_t)(n * n)] = r[(size_t)(2 * n)] * std::sqrt(2.0) / c;
            }
            M.push_back(std::move(row));
        }
        return M;
    }
    double rE = std::cos(137.9 * pi / 180.0 / (order + 1.51)); std::vector<double> pw((size_t)order + 1);
    double p0 = 1, p1 = rE;
    for (int n = 0; n <= order; n++) { double pn = n == 0 ? 1.0 : n == 1 ? rE : ((2 * n - 1) * rE * p1 - (n - 1) * p0) / n; if (n >= 2) { p0 = p1; p1 = pn; } pw[(size_t)n] = pn; }
    std::vector<std::vector<double>> Y;                            // N3D: sqrt(2n + 1) times SN3D
    for (auto& d : dirs) {
        auto g = sh_gains(order, d.first, d.second); std::vector<double> y(nch);
        for (int n = 0; n <= order; n++) for (int m = -n; m <= n; m++) { size_t k = (size_t)(n * n + n + m); y[k] = g[k] * std::sqrt(2.0 * n + 1); }
        Y.push_back(y);
    }
    std::vector<double> w(nch);
    for (int n = 0; n <= order; n++) for (int m = -n; m <= n; m++) w[(size_t)(n * n + n + m)] = pw[(size_t)n];
    auto D = ambi_detail::epad(Y, w);
    for (auto& r : D) {                                            // the N3D input of an SN3D channel: sqrt(2n + 1) B
        std::vector<double> row(nch);
        for (int n = 0; n <= order; n++) for (int m = -n; m <= n; m++) { size_t k = (size_t)(n * n + n + m); row[k] = r[k] * std::sqrt(2.0 * n + 1); }
        M.push_back(std::move(row));
    }
    return M;
}
}  // namespace dsp
}  // namespace musil
