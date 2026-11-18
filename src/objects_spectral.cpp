// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_spectral.cpp — spectral envelope (specenv~), for pfft~ patches
//
// The envelope is Musil's spectral-envelope: the "true envelope", a cepstral
// smoothing of the log spectrum iterated so that it rides on the harmonic
// peaks instead of averaging peaks and valleys.

#include "objects.h"
#include "signals/fft.h"   // Musil's FFT

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
        auto w = std::make_unique<Work>();
        w->fft.init(M);
        w->target.assign((size_t)M, 0.f);
        w->cur.assign((size_t)M, 0.f);
        w->re.assign((size_t)M, 0.f);
        w->im.assign((size_t)M, 0.f);
        work_.set(patch, std::move(w));
    }
    void dsp(int n) override {
        const float* amp = sigIn(0, n);
        float *env = sigOut(0), *fine = sigOut(1);
        Work* w = work_.get();
        if (!w) { dsp::fill(env, 0.f, n); dsp::fill(fine, 0.f, n); return; }
        auto& fft_ = w->fft;
        std::vector<float>&target_ = w->target, &cur_ = w->cur, &re_ = w->re;
        (void)fft_;
        const int M = (int)target_.size();          // 2 frames, or more for a block shorter than the vector
        n = std::min(n, M / 2);
        float peak = 0.f;
        for (int i = 0; i < n; ++i) peak = std::max(peak, amp[i]);
        const float floorv = std::max(peak * 1e-4f, 1e-12f);
        // the log spectrum, mirrored onto the negative frequencies
        for (int k = 0; k < M; ++k) {
            int b = std::min(k <= M / 2 ? k : M - k, n - 1);
            target_[(size_t)k] = std::log(std::max(amp[b], floorv));
        }
        const int order = std::min(order_.load(), M / 2 - 1);
        smooth(*w, target_.data(), cur_.data(), order);
        for (int it = iters_.load(); it > 0; --it) {      // ride on the peaks
            for (int k = 0; k < M; ++k) re_[(size_t)k] = std::max(target_[(size_t)k], cur_[(size_t)k]);
            smooth(*w, re_.data(), cur_.data(), order);
        }
        for (int i = 0; i < n; ++i) {
            float e = std::exp(cur_[(size_t)i]);
            env[i] = e;
            fine[i] = amp[i] / std::max(e, 1e-12f);
        }
    }

private:
    struct Work {
        musil::dsp::FFT<float> fft;
        std::vector<float> target, cur, re, im;
    };
    // keeps the first `order` cepstral coefficients of the log spectrum in
    static void smooth(Work& w, const float* in, float* out, int order) {
        auto& fft_ = w.fft;
        std::vector<float>&re_ = w.re, &im_ = w.im;
        const int M = (int)w.target.size();
        for (int k = 0; k < M; ++k) { re_[(size_t)k] = in[k]; im_[(size_t)k] = 0.f; }
        fft_.inverse(re_.data(), im_.data());            // the real cepstrum (times M)
        for (int k = order + 1; k < M - order; ++k) re_[(size_t)k] = im_[(size_t)k] = 0.f;
        fft_.forward(re_.data(), im_.data());
        for (int k = 0; k < M; ++k) out[k] = re_[(size_t)k] / M;
    }

    std::atomic<int> order_, iters_;
    Published<Work> work_;
    int size_ = 0;                                  // GUI side
};

}  // namespace

void registerSpectralObjects(ClassTable& t) {
    t.insert(t.end(), {
        {"specenv~", "Spectral", "spectral envelope in pfft~: [order] [iterations]", with<SpecenvBox>()},
    });
}

}  // namespace moritz
