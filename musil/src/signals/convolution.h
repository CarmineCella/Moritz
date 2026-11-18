// convolution.h — convolution with long impulse responses: the zero-latency partitioned engine of
// Moritz's conv~, and whole-signal convolution and deconvolution (Musil's deconv, Moritz's condec).
// Self-contained (the standard library and fft.h).
//
// Copyright (c) 2024-2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "fft.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace musil {
namespace dsp {

// PartConv (after pconv's BlockConv): the impulse response is cut into partitions of B samples. The
// first one is applied directly, sample by sample, so the output has no delay; the others use
// uniformly partitioned FFT convolution (overlap-save, FFT size 2B): partition p only needs input
// that is at least p blocks old, so every B samples the whole tail for the next block is computed
// from the blocks already complete.
template <class T>
class PartConv {
public:
    static constexpr int B = 256;

    // ir: len samples, every `stride`-th sample (a channel of interleaved data)
    void setIR(const T* ir, long len, int stride, T gain) {
        len = std::max(0L, len);
        head_.assign(B, (T)0);
        for (long k = 0; k < std::min<long>(B, len); ++k) head_[(size_t)k] = ir[k * stride] * gain;
        P_ = len > B ? (int)((len - B + B - 1) / B) : 0;          // tail partitions
        fft_.init(2 * B);
        H_.assign((size_t)P_, Spec());
        for (int p = 0; p < P_; ++p) {
            std::vector<T> re(2 * B, (T)0), im(2 * B, (T)0);
            for (int k = 0; k < B; ++k) {
                long t = (long)(p + 1) * B + k;
                if (t < len) re[(size_t)k] = ir[t * stride] * gain;
            }
            fft_.forward(re.data(), im.data());
            H_[(size_t)p].re.assign(re.begin(), re.begin() + B + 1);
            H_[(size_t)p].im.assign(im.begin(), im.begin() + B + 1);
        }
        fdl_.assign((size_t)std::max(1, P_), Spec{std::vector<T>(B + 1, (T)0), std::vector<T>(B + 1, (T)0)});
        hist_.assign(2 * B, (T)0);
        prevBlock_.assign(B, (T)0);
        curBlock_.assign(B, (T)0);
        tail_.assign(B, (T)0);
        fill_ = 0;
        fdlPos_ = 0;
        histPos_ = 0;
        re_.assign(2 * B, (T)0);
        im_.assign(2 * B, (T)0);
        ready_ = true;
    }
    bool ready() const { return ready_; }
    long latency() const { return 0; }

    void process(const T* in, T* out, int n) {
        for (int i = 0; i < n; ++i) {
            T x = in[i];
            // head: direct convolution over the last B inputs (history kept twice for a straight loop)
            hist_[(size_t)histPos_] = x;
            hist_[(size_t)histPos_ + B] = x;
            const T* h = hist_.data() + histPos_ + B;   // h[-k] = x[n - k]
            T y = 0;
            for (int k = 0; k < B; ++k) y += head_[(size_t)k] * h[-k];
            histPos_ = (histPos_ + 1) % B;
            out[i] = y + tail_[(size_t)fill_];
            curBlock_[(size_t)fill_] = x;
            if (++fill_ == B) blockDone();
        }
    }

private:
    struct Spec {
        std::vector<T> re, im;
    };
    // a block of input is complete: add its spectrum to the delay line and
    // compute the tail for the next block
    void blockDone() {
        fill_ = 0;
        if (P_ == 0) { std::fill(tail_.begin(), tail_.end(), (T)0); prevBlock_ = curBlock_; return; }
        std::copy(prevBlock_.begin(), prevBlock_.end(), re_.begin());
        std::copy(curBlock_.begin(), curBlock_.end(), re_.begin() + B);
        std::fill(im_.begin(), im_.end(), (T)0);
        fft_.forward(re_.data(), im_.data());
        Spec& X = fdl_[(size_t)fdlPos_];
        std::copy(re_.begin(), re_.begin() + B + 1, X.re.begin());
        std::copy(im_.begin(), im_.begin() + B + 1, X.im.begin());
        prevBlock_ = curBlock_;
        // next block j: sum over p >= 1 of X[j - p] H[p - 1]; X[j - 1] is the block just done
        std::fill(re_.begin(), re_.end(), (T)0);
        std::fill(im_.begin(), im_.end(), (T)0);
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
        for (int k = 0; k < B; ++k) tail_[(size_t)k] = re_[(size_t)(B + k)] / (T)(2 * B);   // overlap-save
        fdlPos_ = (fdlPos_ + 1) % L;
    }

    FFT<T> fft_;
    int P_ = 0, fill_ = 0, fdlPos_ = 0, histPos_ = 0;
    bool ready_ = false;
    std::vector<T> head_, hist_, prevBlock_, curBlock_, tail_, re_, im_;
    std::vector<Spec> H_, fdl_;
};


// y = x * h, the whole linear convolution (nx + nh - 1 samples), by one FFT
inline std::vector<double> convolve(const std::vector<double>& x, const std::vector<double>& h) {
    if (x.empty() || h.empty()) return {};
    size_t len = x.size() + h.size() - 1, N = next_pow2(len);
    std::vector<double> xr(N, 0.0), xi(N, 0.0), hr(N, 0.0), hi(N, 0.0);
    std::copy(x.begin(), x.end(), xr.begin());
    std::copy(h.begin(), h.end(), hr.begin());
    FFT<double> fft;
    fft.init((int)N);
    fft.forward(xr.data(), xi.data());
    fft.forward(hr.data(), hi.data());
    for (size_t k = 0; k < N; ++k) {
        double a = xr[k], b = xi[k];
        xr[k] = a * hr[k] - b * hi[k];
        xi[k] = a * hi[k] + b * hr[k];
    }
    fft.inverse(xr.data(), xi.data());
    std::vector<double> y(len);
    for (size_t k = 0; k < len; ++k) y[k] = xr[k] / (double)N;
    return y;
}

// x such that x * h ~ y (as long as y): the spectrum of y divided by that of h where |H| is above
// threshold times its peak, else 0 (condec). Non-finite results are 0.
inline std::vector<double> deconvolve(const std::vector<double>& y, const std::vector<double>& h, double threshold) {
    if (y.empty() || h.empty()) return std::vector<double>(y.size(), 0.0);
    size_t N = next_pow2(std::max(y.size(), h.size()));
    std::vector<double> yr(N, 0.0), yi(N, 0.0), hr(N, 0.0), hi(N, 0.0);
    std::copy(y.begin(), y.end(), yr.begin());
    std::copy(h.begin(), h.end(), hr.begin());
    FFT<double> fft;
    fft.init((int)N);
    fft.forward(yr.data(), yi.data());
    fft.forward(hr.data(), hi.data());
    double mx = 0;
    for (size_t k = 0; k < N; ++k) mx = std::max(mx, std::hypot(hr[k], hi[k]));
    for (size_t k = 0; k < N; ++k) {
        double ar = yr[k], ai = yi[k], br = hr[k], bi = hi[k];
        if (std::hypot(br, bi) > threshold * mx) {
            double d = br * br + bi * bi;
            yr[k] = (ar * br + ai * bi) / d;
            yi[k] = (ai * br - ar * bi) / d;
        } else {
            yr[k] = yi[k] = 0;
        }
    }
    fft.inverse(yr.data(), yi.data());
    std::vector<double> x(y.size());
    for (size_t k = 0; k < x.size(); ++k) { double v = yr[k] / (double)N; x[k] = std::isfinite(v) ? v : 0.0; }
    return x;
}

}  // namespace dsp
}  // namespace musil
