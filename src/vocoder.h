// Copyright (c) 2011-2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// vocoder.h — the phase vocoder of sparkle (sound_math's BlockVocoder),
// adapted for sparkle~: formant (spectral envelope) preservation through the
// cepstrum, phase locking around spectral peaks, denoising, the phase
// effects (robot, whisper) and sparkle's three cross-syntheses with a second
// sound: 1 multiplicative (the geometric mean of the two magnitudes, phases
// mixed), 2 spectral flattener (the second sound's envelope imposed on the
// first), 3 morphing (magnitudes interpolated, phases switching at the half).
//
// The original measures each bin's frequency from the previous call, which
// assumes the analysis advances by a steady hop. sparkle~ can freeze, play
// backwards or jump, so here every call analyses two frames D samples apart
// (the current one and one just before it) and takes the frequency from
// their phase difference. Synthesis phases still accumulate from call to
// call, so phase locking works as in sparkle.
#pragma once

#include "dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace moritz {

// Complex FFT on interleaved (re, im) arrays of n points; the inverse is
// unscaled, as in sound_math.
class CplxFFT {
public:
    void init(int n) {
        n_ = n;
        fft_.init(n);
        re_.assign((size_t)n, 0.f);
        im_.assign((size_t)n, 0.f);
    }
    void forward(float* c) { split(c); fft_.forward(re_.data(), im_.data()); join(c); }
    void inverse(float* c) { split(c); fft_.inverse(re_.data(), im_.data()); join(c); }

private:
    void split(const float* c) {
        for (int i = 0; i < n_; ++i) { re_[(size_t)i] = c[2 * i]; im_[(size_t)i] = c[2 * i + 1]; }
    }
    void join(float* c) const {
        for (int i = 0; i < n_; ++i) { c[2 * i] = re_[(size_t)i]; c[2 * i + 1] = im_[(size_t)i]; }
    }
    int n_ = 0;
    dsp::FFT fft_;
    std::vector<float> re_, im_;
};

class Vocoder {
public:
    // n: FFT size, a power of two. Frames are 2n floats (interleaved complex).
    explicit Vocoder(int n)
        : N(n), NN(2 * n), N2(n / 2), sicvt(2.0 * dsp::kPi / n),
          peaks((size_t)n, 0), psi((size_t)n, 0.f), phaseInc((size_t)n, 0.f), oldSynPhi((size_t)n, 0.f),
          tmp1((size_t)2 * n, 0.f), tmp2((size_t)2 * n, 0.f), tmp3((size_t)2 * n, 0.f) {
        fft.init(n);
    }

    int phaseMode = 0;    // 1 = robot (zero phases), 2 = whisper (random phases)
    // cross-synthesis: crossMode 1-3 (0 off), crossAmount 0..1; otherNow and
    // otherPrev are the second sound's frames, prepared like frame and prev
    int crossMode = 0;
    float crossAmount = 0.f;
    float* otherNow = nullptr;
    float* otherPrev = nullptr;

    // frame: the windowed analysis frame at the current position (real samples
    // in the even slots, centred); prev: the same, D samples earlier. Both are
    // overwritten. out receives the synthesis frame, to be windowed and
    // overlap-added. P = pitch ratio, F = formant ratio, I = synthesis hop,
    // C = cepstral coefficients for the envelope (0 = no preservation),
    // threshold = denoising (bins below it are dropped).
    void process(float* frame, float* prev, float* out, float P, float F, float D, float I, int C, float threshold) {
        // analysis: phases of the earlier frame, then amplitudes and phases now
        fftshift(prev);
        fft.forward(prev);
        rect2pol(prev);
        fftshift(frame);
        fft.forward(frame);
        rect2pol(frame);
        float* w = frame;
        if (crossMode >= 1 && crossMode <= 3 && otherNow && otherPrev) cross(w, prev, C);

        // phase increment of every bin: its frequency, from the two frames
        for (int i = 0; i < N2; ++i) {
            float omega = (float)(sicvt * i * D);
            float delta = princarg(w[2 * i + 1] - prev[2 * i + 1] - omega);
            phaseInc[(size_t)i] = P * (omega + delta) / D;
        }

        // peaks advance from their last synthesis phase; the bins around a
        // peak keep their analysed offset from it (phase locking)
        int npk = locmax2(w, N2);
        if (npk == 0) npk = 1;
        std::fill(psi.begin(), psi.end(), 0.f);
        for (int k = 0; k < npk; ++k) {
            int p = peaks[(size_t)k];
            psi[(size_t)p] = oldSynPhi[(size_t)p] + I * phaseInc[(size_t)p];
        }
        int start = 0;
        for (int k = 0; k < npk - 1; ++k) {
            int pk = peaks[(size_t)k], next = peaks[(size_t)k + 1];
            int end = (int)std::round((pk + next) * .5f);
            float rot = psi[(size_t)pk] - w[2 * pk + 1];
            for (int j = start; j <= end; ++j)
                if (j != pk) psi[(size_t)j] = rot + w[2 * j + 1];
            start = end + 1;
        }
        for (int i = 0; i < N2; ++i) oldSynPhi[(size_t)i] = psi[(size_t)i];

        // envelope preservation: bin k will sound at k * P, where the
        // envelope should be the original one at k * P / F; scale it by the
        // ratio of the two (the cepstral envelope E is a log, halved by the
        // liftering, hence the 2). sparkle takes the same ratio from the
        // spectrum and a transposed copy of it; reading it off the smooth
        // envelope avoids the empty bins at the edge of the copy (and the
        // P^-15 compensation they needed).
        if (C > 0) {
            for (int i = 0; i < N; ++i) {
                out[2 * i] = std::log(w[2 * i] / NN + kEps);
                out[2 * i + 1] = 0.f;
            }
            cepstralEnvelope(C, out, tmp1.data());
            auto env = [&](float bin) {
                bin = std::clamp(bin, 0.f, (float)(N2 - 1));
                int k = (int)bin;
                float t = bin - k;
                return tmp1[2 * (size_t)k] + t * (tmp1[2 * (size_t)std::min(k + 1, N2 - 1)] - tmp1[2 * (size_t)k]);
            };
            for (int k = 0; k < N2; ++k) w[2 * k] *= std::exp(2.f * (env(k * P / F) - tmp1[2 * (size_t)k]));
        }

        resample(w, out, P, threshold);

        if (phaseMode == 1)
            for (int i = 0; i < N; ++i) out[2 * i + 1] = 0.f;
        else if (phaseMode == 2)
            for (int i = 0; i < N; ++i) out[2 * i + 1] = (float)(std::rand() % N);

        pol2rect(out);
        fft.inverse(out);
        fftshift(out);
    }

    void reset() { std::fill(oldSynPhi.begin(), oldSynPhi.end(), 0.f); }

    // sparkle's cross-synthesis, on the polar frames of both sounds (now and D
    // samples before, so the frequency analysis sees the mixed phases too)
    void cross(float* w, float* prev, int C) {
        float* o = otherNow;
        float* op = otherPrev;
        fftshift(op);
        fft.forward(op);
        rect2pol(op);
        fftshift(o);
        fft.forward(o);
        rect2pol(o);
        const float A = std::clamp(crossAmount, 0.f, 1.f);
        if (crossMode == 1) {                         // multiplicative
            for (int i = 0; i < N; ++i) {
                w[2 * i] = std::sqrt(w[2 * i] * o[2 * i]);
                w[2 * i + 1] = (1.f - A) * w[2 * i + 1] + A * o[2 * i + 1];
                prev[2 * i + 1] = (1.f - A) * prev[2 * i + 1] + A * op[2 * i + 1];
            }
        } else if (crossMode == 2) {                  // spectral flattener: o's envelope on w
            int order = C > 0 ? C : 40;
            for (int i = 0; i < N; ++i) {
                tmp3[2 * i] = std::log(w[2 * i] / NN + kEps);
                tmp3[2 * i + 1] = 0.f;
            }
            cepstralEnvelope(order, tmp3.data(), tmp1.data());
            for (int i = 0; i < N; ++i) {
                tmp3[2 * i] = std::log(o[2 * i] / NN + kEps);
                tmp3[2 * i + 1] = 0.f;
            }
            cepstralEnvelope(order, tmp3.data(), tmp2.data());
            for (int i = 0; i < N; ++i)
                w[2 * i] *= (1.f - A) + A * std::exp(2.f * (tmp2[2 * i] - tmp1[2 * i]));
        } else {                                      // morphing
            bool theirs = A >= .5f;
            for (int i = 0; i < N; ++i) {
                w[2 * i] += (o[2 * i] - w[2 * i]) * A;
                if (theirs) {
                    w[2 * i + 1] = o[2 * i + 1];
                    prev[2 * i + 1] = op[2 * i + 1];
                }
            }
        }
    }

    static void hann(float* w, int n) {
        for (int i = 0; i < n; ++i) w[i] = (float)(.5 * (1. - std::cos(2.0 * dsp::kPi * i / n)));
    }

private:
    static constexpr double kEps = 1e-13;

    void fftshift(float* c) const {
        for (int i = 0; i < N; ++i) std::swap(c[i], c[i + N]);
    }
    static float princarg(float phi) {
        const float twoPi = (float)(2.0 * dsp::kPi);
        return phi - std::round(phi / twoPi) * twoPi;
    }
    void rect2pol(float* c) const {
        for (int i = 0; i < N; ++i) {
            float re = c[2 * i], im = c[2 * i + 1];
            c[2 * i] = std::sqrt(re * re + im * im);
            c[2 * i + 1] = std::atan2(im, re);
        }
    }
    void pol2rect(float* c) const {
        for (int i = 0; i < N; ++i) {
            float a = c[2 * i], ph = c[2 * i + 1];
            c[2 * i] = a * std::cos(ph);
            c[2 * i + 1] = a * std::sin(ph);
        }
    }
    // local maxima over five bins, into peaks
    int locmax2(const float* c, int n) {
        int count = 0;
        for (int i = 2; i < n - 2; ++i) {
            float m = c[2 * i];
            if (m > c[2 * (i - 1)] && m > c[2 * (i + 1)] && m > c[2 * (i - 2)] && m > c[2 * (i + 2)])
                peaks[(size_t)count++] = i;
        }
        return count;
    }
    // frequency-domain resampling of the amplitudes by ratio, with the
    // synthesis phases; mirrored for a real output
    void resample(const float* w, float* out, float ratio, float threshold) {
        std::fill(out, out + NN, 0.f);
        int k = 1;
        int pos = (int)(k * ratio);
        while (pos < N2 && k < N2) {                 // positive frequencies only
            out[2 * pos] = w[2 * k] <= threshold ? 0.f : w[2 * k];
            out[2 * pos + 1] = psi[(size_t)k];
            ++k;
            pos = (int)(k * ratio);
        }
        for (int i = 0; i < N2; ++i) {
            int p = 2 * (N - i) - 2;
            out[p] = out[2 * i];
            out[p + 1] = -out[2 * i + 1];
        }
    }
    // smooth spectral envelope of a log spectrum: keep the first C cepstral
    // coefficients (liftering)
    void cepstralEnvelope(int C, const float* in, float* env) {
        std::copy(in, in + NN, env);
        fft.inverse(env);
        for (int i = 0; i < N; ++i) {
            env[2 * i] /= N;
            env[2 * i + 1] = 0.f;
        }
        env[0] *= .5f;
        for (int i = C + 1; i < N; ++i) env[2 * i] = 0.f;
        fft.forward(env);
    }

    int N, NN, N2;
    double sicvt;
    CplxFFT fft;
    std::vector<int> peaks;
    std::vector<float> psi, phaseInc, oldSynPhi, tmp1, tmp2, tmp3;
};

}  // namespace moritz
