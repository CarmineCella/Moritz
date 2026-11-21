// iir.h — the recursive (IIR) filter of Musil's iir, and of the filters built on it (lowpass, highpass, the
// loudness curve of morphology.h): a0 y[n] = sum b[k] x[n-k] - sum a[k>0] y[n-k]. Self-contained.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "denormal.h"

#include <cstddef>
#include <vector>

namespace musil {

struct iir_state { std::vector<double> xs, ys; };   // the last nb inputs and na outputs, newest first
inline void iir_run(const double* x, double* y, size_t n, const double* b, size_t nb, const double* a, size_t na, iir_state& st) {
    dsp::DenormalGuard ftz;                                // the decaying tail: no denormals (x86)
    if (st.xs.size() != nb) st.xs.assign(nb, 0.0);
    if (st.ys.size() != na) st.ys.assign(na, 0.0);
    for (size_t k = 0; k < n; k++) {
        for (size_t j = nb - 1; j > 0; j--) st.xs[j] = st.xs[j - 1];
        st.xs[0] = x[k];
        double acc = 0;
        for (size_t j = 0; j < nb; j++) acc += b[j] * st.xs[j];
        for (size_t j = 1; j < na; j++) acc -= a[j] * st.ys[j];
        double out = acc / a[0];
        for (size_t j = na - 1; j > 0; j--) st.ys[j] = st.ys[j - 1];
        st.ys[1 < na ? 1 : 0] = out; st.ys[0] = out;
        y[k] = out;
    }
}

}  // namespace musil
