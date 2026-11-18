// nmf.h — non-negative matrix factorisation: Musil's nmf and nmf-with-parts, and the factorisation
// of Moritz's nmf object. Self-contained (the standard library only).
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// V (F x N, non-negative) ~ W (F x K) H (K x N), both non-negative, by Lee & Seung's multiplicative
// updates for the KL divergence (the usual choice for magnitude spectrograms): columns of W are the
// parts (spectral shapes), rows of H their activations over time. After each update of W its
// columns are scaled to unit sum and the scale moved into H, so parts are comparable. Matrices are
// row-major: V[f * N + n].
#pragma once

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

// a starting point: values in [0.5, 1) from raw mt19937 draws (the same on every system)
template <class T>
inline void nmf_init(std::vector<T>& v, unsigned seed) {
    std::mt19937 g(seed);
    for (auto& x : v) x = (T)(0.5 + 0.5 * (g() / 4294967296.0));
}

// iters updates of H, and of W unless fixW (then W stays as given: supervised NMF, parts learnt
// before). tick(it) is called after each iteration; when it returns false the factorisation stops
// and nmf_kl returns false.
template <class T, class Tick>
inline bool nmf_kl(const std::vector<T>& V, int F, int N, int K, int iters, std::vector<T>& W, std::vector<T>& H,
                   bool fixW, Tick tick) {
    const T eps = (T)1e-9;
    std::vector<T> Q((size_t)F * N);
    auto quotient = [&] {                    // Q = V / (W H)
        for (int f = 0; f < F; ++f)
            for (int n = 0; n < N; ++n) {
                T v = 0;
                for (int k = 0; k < K; ++k) v += W[(size_t)f * K + k] * H[(size_t)k * N + n];
                Q[(size_t)f * N + n] = V[(size_t)f * N + n] / (v + eps);
            }
    };
    for (int it = 0; it < iters; ++it) {
        // H <- H * (W^T Q) / (W^T 1)
        quotient();
        for (int k = 0; k < K; ++k) {
            T colsum = 0;
            for (int f = 0; f < F; ++f) colsum += W[(size_t)f * K + k];
            for (int n = 0; n < N; ++n) {
                T num = 0;
                for (int f = 0; f < F; ++f) num += W[(size_t)f * K + k] * Q[(size_t)f * N + n];
                H[(size_t)k * N + n] *= num / (colsum + eps);
            }
        }
        if (!fixW) {
            // W <- W * (Q H^T) / (1 H^T)
            quotient();
            for (int k = 0; k < K; ++k) {
                T rowsum = 0;
                for (int n = 0; n < N; ++n) rowsum += H[(size_t)k * N + n];
                for (int f = 0; f < F; ++f) {
                    T num = 0;
                    for (int n = 0; n < N; ++n) num += Q[(size_t)f * N + n] * H[(size_t)k * N + n];
                    W[(size_t)f * K + k] *= num / (rowsum + eps);
                }
            }
            for (int k = 0; k < K; ++k) {          // parts of unit sum, the scale in H
                T sum = 0;
                for (int f = 0; f < F; ++f) sum += W[(size_t)f * K + k];
                sum = std::max(sum, eps);
                for (int f = 0; f < F; ++f) W[(size_t)f * K + k] /= sum;
                for (int n = 0; n < N; ++n) H[(size_t)k * N + n] *= sum;
            }
        }
        if (!tick(it)) return false;
    }
    return true;
}
