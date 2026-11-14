// dsp.h — signal-processing building blocks for minlsp
//
// Self-contained: no GUI, no audio-device code. Every unit processes one
// block of at most kBlock samples. Units are driven from the audio thread;
// anything the GUI thread may touch while audio runs is atomic.

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace dsp {

constexpr int kBlock = 64;                    // internal vector size
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// Turn on flush-to-zero so recursive filters never hit slow denormals.
inline void flushDenormals() {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_setcsr(_mm_getcsr() | 0x8040);         // FTZ | DAZ
#elif defined(__aarch64__)
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    asm volatile("msr fpcr, %0" ::"r"(fpcr | (1ULL << 24)));  // FZ
#endif
}

inline void fill(float* out, float v, int n) { std::fill(out, out + n, v); }

// ------------------------------------------------------------------ cycle~
// Cosine table with a guard point, shared by all oscillators (as in MSP's
// cycle~, 512 points, linear interpolation).
struct CosTable {
    static constexpr int N = 512;
    float t[N + 1];
    CosTable() {
        for (int i = 0; i <= N; ++i) t[i] = (float)std::cos(kTwoPi * i / N);
    }
    static const CosTable& get() {
        static const CosTable table;
        return table;
    }
};

class Cycle {
public:
    // Sets the phase (0..1); safe to call from the GUI thread.
    void setPhase(float p) { pendingPhase_.store(p - std::floor(p)); }

    void process(const float* freq, float* out, int n, double sr) {
        float p = pendingPhase_.exchange(-1.f);
        if (p >= 0.f) phase_ = p;
        const float* t = CosTable::get().t;
        const double inv = 1.0 / sr;
        for (int i = 0; i < n; ++i) {
            double x = phase_ * CosTable::N;
            int idx = (int)x;
            float frac = (float)(x - idx);
            out[i] = t[idx] + frac * (t[idx + 1] - t[idx]);
            phase_ += freq[i] * inv;
            phase_ -= std::floor(phase_);
            if (!(phase_ >= 0.0 && phase_ < 1.0)) phase_ = 0.0;  // NaN/inf guard
        }
    }

private:
    double phase_ = 0.0;
    std::atomic<float> pendingPhase_{-1.f};
};

// ------------------------------------------------------------------ reson~
// Two-pole resonant bandpass, the same structure as MSP's reson~:
//   y[n] = g*(1-r)*(x[n] - r*x[n-2]) + 2r*cos(w)*y[n-1] - r^2*y[n-2]
// with r = exp(-pi*bw/sr), bw = cf/Q. Scaling by (1-r) gives exactly unity
// gain at the centre frequency, so `gain` is the peak gain.
class Reson {
public:
    // Coefficients are computed once per block from the first sample of the
    // gain/cf/q inputs (control-rate update, like most MSP filters).
    void process(const float* in, float* out, int n, float gain, float cf, float q,
                 double sr) {
        if (cf != cf_ || q != q_ || sr != sr_) update(cf, q, sr);
        const double g = gain * scale_;
        double x1 = x1_, x2 = x2_, y1 = y1_, y2 = y2_;
        for (int i = 0; i < n; ++i) {
            double x = in[i];
            double y = g * (x - r_ * x2) + c1_ * y1 + c2_ * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            out[i] = (float)y;
        }
        if (!std::isfinite(y1) || !std::isfinite(y2)) y1 = y2 = x1 = x2 = 0.0;
        x1_ = x1; x2_ = x2; y1_ = y1; y2_ = y2;
    }

    void clear() { x1_ = x2_ = y1_ = y2_ = 0.0; }

private:
    void update(float cf, float q, double sr) {
        cf_ = cf; q_ = q; sr_ = sr;
        double f = std::clamp((double)cf, 1.0, sr * 0.49);
        double qq = std::max((double)q, 0.01);
        double bw = f / qq;
        r_ = std::exp(-kPi * bw / sr);
        c1_ = 2.0 * r_ * std::cos(kTwoPi * f / sr);
        c2_ = -r_ * r_;
        scale_ = 1.0 - r_;
    }

    float cf_ = -1.f, q_ = -1.f;
    double sr_ = 0.0;
    double r_ = 0.0, c1_ = 0.0, c2_ = 0.0, scale_ = 1.0;
    double x1_ = 0.0, x2_ = 0.0, y1_ = 0.0, y2_ = 0.0;
};

// ------------------------------------------------------------------ noise~
// White noise in [-1, 1) from a xorshift32 generator.
class Noise {
public:
    explicit Noise(uint32_t seed = 0x9E3779B9u) : s_(seed ? seed : 1u) {}
    void process(float* out, int n) {
        for (int i = 0; i < n; ++i) {
            s_ ^= s_ << 13; s_ ^= s_ >> 17; s_ ^= s_ << 5;
            out[i] = (float)((int32_t)s_) * (1.0f / 2147483648.0f);
        }
    }

private:
    uint32_t s_;
};

// ------------------------------------------------------------------ click~
// Plays its impulse shape (default: a single sample of 1.0) each time it is
// triggered. trigger() may be called from any thread; setShape() must be
// called while the audio thread is excluded (the patch's DSP lock).
class Click {
public:
    void trigger() { pending_.fetch_add(1); }
    void setShape(std::vector<float> s) {
        shape_ = s.empty() ? std::vector<float>{1.f} : std::move(s);
        pos_ = shape_.size();
    }

    void process(float* out, int n) {
        if (pending_.exchange(0) > 0) pos_ = 0;
        for (int i = 0; i < n; ++i)
            out[i] = pos_ < shape_.size() ? shape_[pos_++] : 0.f;
    }

private:
    std::vector<float> shape_{1.f};
    size_t pos_ = 1;
    std::atomic<int> pending_{0};
};

// ------------------------------------------------------------------ arithmetic
enum class Op { Add, Sub, Mul, Div };

inline float apply(Op op, float a, float b) {
    switch (op) {
        case Op::Add: return a + b;
        case Op::Sub: return a - b;
        case Op::Mul: return a * b;
        case Op::Div: return b == 0.f ? 0.f : a / b;
    }
    return 0.f;
}

inline void binop(Op op, const float* a, const float* b, float* out, int n) {
    switch (op) {
        case Op::Add: for (int i = 0; i < n; ++i) out[i] = a[i] + b[i]; break;
        case Op::Sub: for (int i = 0; i < n; ++i) out[i] = a[i] - b[i]; break;
        case Op::Mul: for (int i = 0; i < n; ++i) out[i] = a[i] * b[i]; break;
        case Op::Div:
            for (int i = 0; i < n; ++i) out[i] = b[i] == 0.f ? 0.f : a[i] / b[i];
            break;
    }
}

}  // namespace dsp
