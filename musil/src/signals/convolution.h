// convolution.h — convolution with long impulse responses: the zero-latency partitioned engine of
// Moritz's conv~, and whole-signal convolution and deconvolution (Musil's deconv, Moritz's condec).
// Self-contained (the standard library and fft.h).
//
// Copyright (c) 2024-2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "denormal.h"
#include "fft.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace musil {
namespace dsp {

// PartConv (after pconv's BlockConv): the impulse response is cut into partitions of B samples. The
// first one is applied directly, sample by sample, so the output has no delay; the others use
// uniformly partitioned FFT convolution (overlap-save, real FFTs of 2B): partition p only needs input
// that is at least p blocks old. The tail of the next block is the sum over the partitions of the
// spectra of the blocks before times theirs; all its terms but the newest block's are known while
// the current block fills, so they are accumulated a share at a time as its samples come in (the
// work is spread over the callbacks of a block instead of landing in one), and only the newest
// block's FFT, its product with the first partition and the inverse FFT are left for the block's end.
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
        std::vector<T> buf(2 * B);
        for (int p = 0; p < P_; ++p) {
            std::fill(buf.begin(), buf.end(), (T)0);
            for (int k = 0; k < B; ++k) {
                long t = (long)(p + 1) * B + k;
                if (t < len) buf[(size_t)k] = ir[t * stride] * gain;
            }
            H_[(size_t)p].re.assign(B + 1, (T)0);
            H_[(size_t)p].im.assign(B + 1, (T)0);
            fft_.forward(buf.data(), H_[(size_t)p].re.data(), H_[(size_t)p].im.data());
        }
        fdl_.assign((size_t)std::max(1, P_), Spec{std::vector<T>(B + 1, (T)0), std::vector<T>(B + 1, (T)0)});
        hist_.assign(2 * B, (T)0);
        prevBlock_.assign(B, (T)0);
        curBlock_.assign(B, (T)0);
        tail_.assign(B, (T)0);
        fill_ = 0;
        fdlPos_ = 0;
        histPos_ = 0;
        done_ = 1;
        buf_.assign(2 * B, (T)0);
        accRe_.assign(B + 1, (T)0);
        accIm_.assign(B + 1, (T)0);
        ready_ = true;
    }
    bool ready() const { return ready_; }
    long latency() const { return 0; }

    void process(const T* in, T* out, int n) {
        DenormalGuard ftz;                             // a reverberation tail decays through denormals (x86)
        int i = 0;
        while (i < n) {
            int run = std::min(n - i, B - fill_);          // up to the end of the block
            for (int e = i + run; i < e; ++i) {
                T x = in[i];
                // head: direct convolution over the last B inputs (history kept twice for a straight loop)
                hist_[(size_t)histPos_] = x;
                hist_[(size_t)histPos_ + B] = x;
                const T* h = hist_.data() + histPos_ + B;   // h[-k] = x[n - k]
                T y = 0;
                for (int k = 0; k < B; ++k) y += head_[(size_t)k] * h[-k];
                histPos_ = (histPos_ + 1) % B;
                out[i] = y + tail_[(size_t)fill_];
                curBlock_[(size_t)fill_++] = x;
            }
            // the share of the older partitions that is due by now
            accumulate(1 + (int)((long)(P_ - 1) * fill_ / B));
            if (fill_ == B) blockDone();
        }
    }

private:
    struct Spec {
        std::vector<T> re, im;
    };
    // the terms X[j - p] H[p] for p in [done_, upto), the blocks already in the delay line (X[j - p] is at
    // fdlPos_ - p: fdlPos_ is where the block filling now goes)
    void accumulate(int upto) {
        upto = std::min(upto, P_);
        const int L = (int)fdl_.size();
        for (; done_ < upto; ++done_) {
            const Spec& x = fdl_[(size_t)((fdlPos_ - done_ + L) % L)];
            const Spec& h = H_[(size_t)done_];
            mac(x, h);
        }
    }
    void mac(const Spec& x, const Spec& h) {
        const T *xr = x.re.data(), *xi = x.im.data(), *hr = h.re.data(), *hi = h.im.data();
        T *ar = accRe_.data(), *ai = accIm_.data();
        for (int k = 0; k <= B; ++k) {
            ar[k] += xr[k] * hr[k] - xi[k] * hi[k];
            ai[k] += xr[k] * hi[k] + xi[k] * hr[k];
        }
    }
    // a block of input is complete: its spectrum into the delay line, its term added, the tail of the next block
    void blockDone() {
        fill_ = 0;
        if (P_ == 0) { std::fill(tail_.begin(), tail_.end(), (T)0); prevBlock_ = curBlock_; return; }
        accumulate(P_);                                   // (all done already, unless a call ended exactly here)
        std::copy(prevBlock_.begin(), prevBlock_.end(), buf_.begin());
        std::copy(curBlock_.begin(), curBlock_.end(), buf_.begin() + B);
        Spec& X = fdl_[(size_t)fdlPos_];
        fft_.forward(buf_.data(), X.re.data(), X.im.data());
        prevBlock_ = curBlock_;
        mac(X, H_[0]);
        fft_.inverse(accRe_.data(), accIm_.data(), buf_.data());
        for (int k = 0; k < B; ++k) tail_[(size_t)k] = buf_[(size_t)(B + k)] / (T)(2 * B);   // overlap-save
        std::fill(accRe_.begin(), accRe_.end(), (T)0);
        std::fill(accIm_.begin(), accIm_.end(), (T)0);
        done_ = 1;
        fdlPos_ = (fdlPos_ + 1) % (int)fdl_.size();
    }

    RealFFT<T> fft_;
    int P_ = 0, fill_ = 0, fdlPos_ = 0, histPos_ = 0, done_ = 1;
    bool ready_ = false;
    std::vector<T> head_, hist_, prevBlock_, curBlock_, tail_, buf_, accRe_, accIm_;
    std::vector<Spec> H_, fdl_;
};

// y = x * h, the whole linear convolution (n + m - 1 samples, n and m the lengths of x and h), by real FFTs:
// one transform when x is not much longer than h, else overlap-add over blocks (FFTs of about 8 times h's
// length). breathe(), when given, is called between blocks (an interpreter keeps its windows alive).
template <class Breathe>
inline std::vector<double> convolve(const double* x, size_t n, const double* h, size_t m, Breathe breathe) {
    if (n == 0 || m == 0) return {};
    size_t len = n + m - 1, N = next_pow2(std::max<size_t>(2, len)), cap = 8 * next_pow2(std::max<size_t>(2, m));
    if (N > cap) N = cap;                                   // overlap-add: blocks of N - m + 1 samples of x
    size_t B = N - m + 1, H = N / 2 + 1;
    RealFFT<double>& f = rfft_plan<double>((int)N);
    std::vector<double> buf(N, 0.0), hr(H), hi(H), xr(H), xi(H), y(len, 0.0);
    std::copy(h, h + m, buf.begin());
    f.forward(buf.data(), hr.data(), hi.data());
    for (size_t start = 0; start < n; start += B) {
        size_t take = std::min(B, n - start);
        std::fill(buf.begin(), buf.end(), 0.0);
        std::copy(x + start, x + start + take, buf.begin());
        f.forward(buf.data(), xr.data(), xi.data());
        for (size_t k = 0; k < H; ++k) {
            double a = xr[k], b = xi[k];
            xr[k] = a * hr[k] - b * hi[k];
            xi[k] = a * hi[k] + b * hr[k];
        }
        f.inverse(xr.data(), xi.data(), buf.data());
        size_t out = std::min(take + m - 1, len - start);
        for (size_t k = 0; k < out; ++k) y[start + k] += buf[k] / (double)N;
        if (start + B < n) breathe();
    }
    return y;
}
inline std::vector<double> convolve(const std::vector<double>& x, const std::vector<double>& h) {
    return convolve(x.data(), x.size(), h.data(), h.size(), [] {});
}

// x such that x * h ~ y (as long as y), by regularised (Tikhonov, Wiener-like) inversion of h's spectrum:
// X = Y conj(H) / (|H|^2 + lambda max|H|^2) with lambda = threshold^2 (condec). Where |H| is well above
// threshold times its peak this is Y / H; at threshold times the peak the gain is half the inverse's, and
// below it falls to 0 smoothly instead of being cut (a hard cut rings). threshold 0: the plain inverse.
// Non-finite results are 0.
inline std::vector<double> deconvolve(const std::vector<double>& y, const std::vector<double>& h, double threshold) {
    if (y.empty() || h.empty()) return std::vector<double>(y.size(), 0.0);
    size_t N = next_pow2(std::max<size_t>(2, std::max(y.size(), h.size()))), H = N / 2 + 1;
    RealFFT<double>& f = rfft_plan<double>((int)N);
    std::vector<double> buf(N, 0.0), yr(H), yi(H), hr(H), hi(H);
    std::copy(y.begin(), y.end(), buf.begin());
    f.forward(buf.data(), yr.data(), yi.data());
    std::fill(buf.begin(), buf.end(), 0.0);
    std::copy(h.begin(), h.end(), buf.begin());
    f.forward(buf.data(), hr.data(), hi.data());
    double mx2 = 0;
    for (size_t k = 0; k < H; ++k) mx2 = std::max(mx2, hr[k] * hr[k] + hi[k] * hi[k]);
    double reg = threshold * threshold * mx2;
    for (size_t k = 0; k < H; ++k) {
        double ar = yr[k], ai = yi[k], br = hr[k], bi = hi[k], d = br * br + bi * bi + reg;
        if (d > 0) { yr[k] = (ar * br + ai * bi) / d; yi[k] = (ai * br - ar * bi) / d; }
        else yr[k] = yi[k] = 0;
    }
    f.inverse(yr.data(), yi.data(), buf.data());
    std::vector<double> x(y.size());
    for (size_t k = 0; k < x.size(); ++k) { double v = buf[k] / (double)N; x[k] = std::isfinite(v) ? v : 0.0; }
    return x;
}

}  // namespace dsp
}  // namespace musil
