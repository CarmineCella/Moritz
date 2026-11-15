// Copyright (c) 2024-2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// partconv.h — zero-latency convolution with a long impulse response, for
// conv~ (after pconv's BlockConv and condec)
//
// The impulse response is cut into partitions of B samples. The first one is
// applied directly, sample by sample, so the output has no delay; the others
// use uniformly partitioned FFT convolution (overlap-save, FFT size 2B):
// partition p only needs input that is at least p blocks old, so every B
// samples the whole tail for the next block is computed from the blocks
// already complete.
#pragma once

#include "dsp.h"

#include <algorithm>
#include <vector>

namespace moritz {

class PartConv {
public:
    static constexpr int B = 256;

    // ir: len samples, every `stride`-th float (a channel of interleaved data)
    void setIR(const float* ir, long len, int stride, float gain) {
        len = std::max(0L, len);
        head_.assign(B, 0.f);
        for (long k = 0; k < std::min<long>(B, len); ++k) head_[(size_t)k] = ir[k * stride] * gain;
        P_ = len > B ? (int)((len - B + B - 1) / B) : 0;          // tail partitions
        fft_.init(2 * B);
        H_.assign((size_t)P_, Spec());
        for (int p = 0; p < P_; ++p) {
            std::vector<float> re(2 * B, 0.f), im(2 * B, 0.f);
            for (int k = 0; k < B; ++k) {
                long t = (long)(p + 1) * B + k;
                if (t < len) re[(size_t)k] = ir[t * stride] * gain;
            }
            fft_.forward(re.data(), im.data());
            H_[(size_t)p].re.assign(re.begin(), re.begin() + B + 1);
            H_[(size_t)p].im.assign(im.begin(), im.begin() + B + 1);
        }
        fdl_.assign((size_t)std::max(1, P_), Spec{std::vector<float>(B + 1, 0.f), std::vector<float>(B + 1, 0.f)});
        hist_.assign(2 * B, 0.f);
        prevBlock_.assign(B, 0.f);
        curBlock_.assign(B, 0.f);
        tail_.assign(B, 0.f);
        fill_ = 0;
        fdlPos_ = 0;
        histPos_ = 0;
        re_.assign(2 * B, 0.f);
        im_.assign(2 * B, 0.f);
        ready_ = true;
    }
    bool ready() const { return ready_; }
    long latency() const { return 0; }

    void process(const float* in, float* out, int n) {
        for (int i = 0; i < n; ++i) {
            float x = in[i];
            // head: direct convolution over the last B inputs (history kept twice for a straight loop)
            hist_[(size_t)histPos_] = x;
            hist_[(size_t)histPos_ + B] = x;
            const float* h = hist_.data() + histPos_ + B;   // h[-k] = x[n - k]
            float y = 0.f;
            for (int k = 0; k < B; ++k) y += head_[(size_t)k] * h[-k];
            histPos_ = (histPos_ + 1) % B;
            out[i] = y + tail_[(size_t)fill_];
            curBlock_[(size_t)fill_] = x;
            if (++fill_ == B) blockDone();
        }
    }

private:
    struct Spec {
        std::vector<float> re, im;
    };
    // a block of input is complete: add its spectrum to the delay line and
    // compute the tail for the next block
    void blockDone() {
        fill_ = 0;
        if (P_ == 0) { std::fill(tail_.begin(), tail_.end(), 0.f); prevBlock_ = curBlock_; return; }
        std::copy(prevBlock_.begin(), prevBlock_.end(), re_.begin());
        std::copy(curBlock_.begin(), curBlock_.end(), re_.begin() + B);
        std::fill(im_.begin(), im_.end(), 0.f);
        fft_.forward(re_.data(), im_.data());
        Spec& X = fdl_[(size_t)fdlPos_];
        std::copy(re_.begin(), re_.begin() + B + 1, X.re.begin());
        std::copy(im_.begin(), im_.begin() + B + 1, X.im.begin());
        prevBlock_ = curBlock_;
        // next block j: sum over p >= 1 of X[j - p] H[p - 1]; X[j - 1] is the block just done
        std::fill(re_.begin(), re_.end(), 0.f);
        std::fill(im_.begin(), im_.end(), 0.f);
        int L = (int)fdl_.size();
        for (int p = 0; p < P_; ++p) {
            const Spec& x = fdl_[(size_t)((fdlPos_ - p + L) % L)];
            const Spec& h = H_[(size_t)p];
            for (int k = 0; k <= B; ++k) {
                re_[(size_t)k] += x.re[(size_t)k] * h.re[(size_t)k] - x.im[(size_t)k] * h.im[(size_t)k];
                im_[(size_t)k] += x.re[(size_t)k] * h.im[(size_t)k] + x.im[(size_t)k] * h.re[(size_t)k];
            }
        }
        for (int k = 1; k < B; ++k) {                       // a real signal: mirror the spectrum
            re_[(size_t)(2 * B - k)] = re_[(size_t)k];
            im_[(size_t)(2 * B - k)] = -im_[(size_t)k];
        }
        fft_.inverse(re_.data(), im_.data());
        for (int k = 0; k < B; ++k) tail_[(size_t)k] = re_[(size_t)(B + k)] / (2 * B);   // overlap-save
        fdlPos_ = (fdlPos_ + 1) % L;
    }

    dsp::FFT fft_;
    int P_ = 0, fill_ = 0, fdlPos_ = 0, histPos_ = 0;
    bool ready_ = false;
    std::vector<float> head_, hist_, prevBlock_, curBlock_, tail_, re_, im_;
    std::vector<Spec> H_, fdl_;
};

}  // namespace moritz
