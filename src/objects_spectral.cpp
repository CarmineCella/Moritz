// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_spectral.cpp — spectral envelope (specenv~), for pfft~ patches
//
// The envelope is Musil's spectral-envelope (signals/envelope.h): the "true envelope", a cepstral
// smoothing of the log spectrum iterated so that it rides on the harmonic
// peaks instead of averaging peaks and valleys.

#include "objects.h"
#include "signals/envelope.h"   // Musil's spectral envelope

#include <algorithm>
#include <cmath>

namespace moritz {
namespace {

int pow2At(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

// ---------------------------------------------------------------- specenv~
// specenv~ [order] [iterations]: inside pfft~, the spectral envelope of the
// amplitudes in its left inlet (one frame at a time), and the fine structure
// (the amplitudes divided by the envelope: the harmonics, or the noise, with
// the formants taken out). order: cepstral coefficients kept, the
// resolution; about sr / (2 f0) follows a voice's formants without rippling
// at its harmonics (40 by default; a number in the right inlet changes it).
// iterations: 0 for the plain cepstral smoothing, which averages peaks and
// valleys; 12 by default, the true envelope.
class SpecenvBox : public Box {
public:
    explicit SpecenvBox(const Message& a)
        : order_(std::max(1, (int)argf(a, 0, 40.f))), iters_(std::clamp((int)argf(a, 1, 12.f), 0, 100)) {
        setPorts({P::Signal, P::Control}, {P::Signal, P::Signal});
    }
    void message(int in, const Message& m) override {
        if (in == 1 && m[0].isFloat()) order_.store(std::max(1, (int)m[0].f));
        else if (m[0].is("iterations")) iters_.store(std::clamp((int)argf(m, 1, 12.f), 0, 100));
    }
    // sized at compile for the frame (pfft~'s bins) and published, never in dsp()
    void resolve() override {
        const int M = pow2At(2 * patch->vectorSize());
        if (M == size_) return;
        size_ = M;
        auto w = std::make_unique<musil::dsp::TrueEnvelope<float>>();
        w->init(M);
        work_.set(patch, std::move(w));
    }
    // Musil's spectral-envelope (signals/envelope.h): the half spectrum mirrored onto M points
    void dsp(int n) override {
        const float* amp = sigIn(0, n);
        float *env = sigOut(0), *fine = sigOut(1);
        musil::dsp::TrueEnvelope<float>* w = work_.get();
        if (!w) { dsp::fill(env, 0.f, n); dsp::fill(fine, 0.f, n); return; }
        n = std::min(n, w->size() / 2);              // 2 frames, or more for a block shorter than the vector
        w->half(amp, n, order_.load(), iters_.load(), env);
        for (int i = 0; i < n; ++i) fine[i] = amp[i] / std::max(env[i], 1e-12f);
    }

private:
    std::atomic<int> order_, iters_;
    Published<musil::dsp::TrueEnvelope<float>> work_;
    int size_ = 0;                                  // GUI side
};

}  // namespace

void registerSpectralObjects(ClassTable& t) {
    t.insert(t.end(), {
        {"specenv~", "Spectral", "spectral envelope in pfft~: [order] [iterations]", with<SpecenvBox>()},
    });
}

}  // namespace moritz
