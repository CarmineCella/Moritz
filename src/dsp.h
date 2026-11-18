// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// dsp.h — signal-processing building blocks for Moritz
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

// Flush-to-zero for the life of the scope, then the thread's mode as it was:
// for the threads that run a patch besides the audio callback (moritz
// --render, the render object's worker).
class FlushDenormalsScope {
public:
    FlushDenormalsScope() {
#if defined(__x86_64__) || defined(_M_X64)
        saved_ = _mm_getcsr();
#elif defined(__aarch64__)
        asm volatile("mrs %0, fpcr" : "=r"(saved_));
#endif
        flushDenormals();
    }
    ~FlushDenormalsScope() {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_setcsr((unsigned)saved_);
#elif defined(__aarch64__)
        asm volatile("msr fpcr, %0" ::"r"(saved_));
#endif
    }
    FlushDenormalsScope(const FlushDenormalsScope&) = delete;
    FlushDenormalsScope& operator=(const FlushDenormalsScope&) = delete;

private:
    uint64_t saved_ = 0;
};

// Whether denormals are flushed on this thread now (for the tests).
inline bool denormalsFlushed() {
#if defined(__x86_64__) || defined(_M_X64)
    return (_mm_getcsr() & 0x8040) == 0x8040;
#elif defined(__aarch64__)
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    return (fpcr & (1ULL << 24)) != 0;
#else
    return true;
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

// ------------------------------------------------------------------ SPSC queue
// Lock-free single-producer / single-consumer queue: GUI thread pushes,
// audio thread pops. No allocation after construction.
template <class T, size_t N>
class Spsc {
public:
    bool push(const T& v) {
        size_t h = head_.load(std::memory_order_relaxed), n = (h + 1) % N;
        if (n == tail_.load(std::memory_order_acquire)) return false;   // full
        buf_[h] = v;
        head_.store(n, std::memory_order_release);
        return true;
    }
    bool pop(T& v) {
        size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        v = buf_[t];
        tail_.store((t + 1) % N, std::memory_order_release);
        return true;
    }

private:
    T buf_[N];
    std::atomic<size_t> head_{0}, tail_{0};
};

// ------------------------------------------------------------------ line~
// Piecewise-linear ramp. The GUI posts Begin (drop pending segments, keep
// the current value), then Segment commands; Stop freezes the ramp.
class Ramp {
public:
    struct Cmd {
        enum Kind : uint8_t { Begin, Segment, Stop } kind = Segment;
        float target = 0.f, ms = 0.f;
    };
    bool post(const Cmd& c) { return q_.push(c); }

    void process(float* out, int n, double sr) {
        Cmd c;
        while (q_.pop(c)) {
            if (c.kind == Cmd::Segment) {
                if (c.ms * sr < 1000.0 && idx_ == count_ && left_ <= 0)
                    value_ = target_ = c.target;   // a jump with nothing pending: now, so a
                                                   // "0, 1 100" pair in one block keeps both
                else if (count_ < kMax)
                    segs_[count_++] = c;
            } else {
                count_ = idx_ = 0;
                left_ = 0;
            }
        }
        for (int i = 0; i < n; ++i) {
            if (left_ <= 0 && idx_ < count_) {
                const Cmd& s = segs_[idx_++];
                double len = s.ms * sr / 1000.0;
                target_ = s.target;
                if (len < 1.0) {
                    value_ = target_;
                } else {
                    left_ = (long)len;
                    inc_ = (target_ - value_) / (double)left_;
                }
            }
            if (left_ > 0 && --left_ == 0) value_ = target_;
            else if (left_ > 0) value_ += inc_;
            out[i] = (float)value_;
        }
    }

private:
    static constexpr int kMax = 64;
    Spsc<Cmd, 256> q_;
    Cmd segs_[kMax];
    int count_ = 0, idx_ = 0;
    long left_ = 0;
    double value_ = 0.0, inc_ = 0.0, target_ = 0.0;
};

// ------------------------------------------------------------------ oscillators
// phasor~: non-band-limited 0..1 ramp (as in MSP; used as a control ramp).
class Phasor {
public:
    void setPhase(float p) { pending_.store(p - std::floor(p)); }
    void process(const float* freq, float* out, int n, double sr) {
        float p = pending_.exchange(-1.f);
        if (p >= 0.f) ph_ = p;
        const double inv = 1.0 / sr;
        for (int i = 0; i < n; ++i) {
            out[i] = (float)ph_;
            ph_ += freq[i] * inv;
            ph_ -= std::floor(ph_);
            if (!(ph_ >= 0.0 && ph_ < 1.0)) ph_ = 0.0;
        }
    }

private:
    double ph_ = 0.0;
    std::atomic<float> pending_{-1.f};
};

// PolyBLEP residual for band-limited discontinuities.
inline double polyBlep(double t, double dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}

// saw~ / rect~ / tri~: band-limited (PolyBLEP) saw and pulse; triangle with
// adjustable slope (its harmonics fall off fast enough to stay naive).
class Osc {
public:
    enum Shape { Saw, Rect, Tri };
    explicit Osc(Shape s) : shape_(s) {}
    void process(const float* freq, const float* duty, float* out, int n, double sr) {
        for (int i = 0; i < n; ++i) {
            double dt = std::min(std::fabs((double)freq[i]) / sr, 0.5);
            double t = ph_;
            double v;
            if (shape_ == Saw) {
                v = 2.0 * t - 1.0 - (dt > 0 ? polyBlep(t, dt) : 0.0);
            } else if (shape_ == Rect) {
                double d = std::clamp((double)duty[i], 0.01, 0.99);
                v = t < d ? 1.0 : -1.0;
                if (dt > 0) {
                    v += polyBlep(t, dt);
                    double t2 = t + 1.0 - d;
                    v -= polyBlep(t2 - std::floor(t2), dt);
                }
            } else {
                double d = std::clamp((double)duty[i], 0.01, 0.99);
                v = t < d ? 2.0 * t / d - 1.0 : 1.0 - 2.0 * (t - d) / (1.0 - d);
            }
            out[i] = (float)v;
            ph_ += freq[i] / sr;
            ph_ -= std::floor(ph_);
            if (!(ph_ >= 0.0 && ph_ < 1.0)) ph_ = 0.0;
        }
    }

private:
    Shape shape_;
    double ph_ = 0.0;
};

// ------------------------------------------------------------------ filters
// Biquad in MSP's convention:
//   y[n] = a0 x[n] + a1 x[n-1] + a2 x[n-2] - b1 y[n-1] - b2 y[n-2]
class Biquad {
public:
    void set(double a0, double a1, double a2, double b1, double b2) {
        a0_ = a0; a1_ = a1; a2_ = a2; b1_ = b1; b2_ = b2;
    }
    // RBJ cookbook low-pass
    void lowpass(double f, double q, double sr) {
        f = std::clamp(f, 1.0, sr * 0.49);
        q = std::max(q, 0.1);
        double w = kTwoPi * f / sr, c = std::cos(w), al = std::sin(w) / (2.0 * q);
        double n = 1.0 / (1.0 + al);
        set((1.0 - c) * 0.5 * n, (1.0 - c) * n, (1.0 - c) * 0.5 * n, -2.0 * c * n,
            (1.0 - al) * n);
    }
    void process(const float* in, float* out, int n) {
        double x1 = x1_, x2 = x2_, y1 = y1_, y2 = y2_;
        for (int i = 0; i < n; ++i) {
            double x = in[i];
            double y = a0_ * x + a1_ * x1 + a2_ * x2 - b1_ * y1 - b2_ * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            out[i] = (float)y;
        }
        if (!std::isfinite(y1) || !std::isfinite(y2)) x1 = x2 = y1 = y2 = 0.0;
        x1_ = x1; x2_ = x2; y1_ = y1; y2_ = y2;
    }

private:
    double a0_ = 1, a1_ = 0, a2_ = 0, b1_ = 0, b2_ = 0;
    double x1_ = 0, x2_ = 0, y1_ = 0, y2_ = 0;
};

// ------------------------------------------------------------------ delay line
// Ring buffer. read(k) returns the sample written k pushes ago, counted
// from the next write position (k = 1 is the most recent sample), with
// linear interpolation for fractional k.
class DelayLine {
public:
    void resize(size_t n) { buf_.assign(std::max<size_t>(n, 4), 0.f); w_ = 0; }
    size_t size() const { return buf_.size(); }
    void push(float x) { buf_[w_] = x; if (++w_ == buf_.size()) w_ = 0; }
    float read(double k) const {
        k = std::clamp(k, 1.0, (double)buf_.size() - 2.0);
        size_t ik = (size_t)k;
        float fr = (float)(k - ik);
        size_t N = buf_.size();
        size_t a = (w_ + N - ik) % N, b = (w_ + 2 * N - ik - 1) % N;
        return buf_[a] + fr * (buf_[b] - buf_[a]);
    }
    void clear() { std::fill(buf_.begin(), buf_.end(), 0.f); }

private:
    std::vector<float> buf_;
    size_t w_ = 0;
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

// ------------------------------------------------------------------ FFT
// In-place iterative radix-2 complex FFT. inverse() is unscaled, so
// inverse(forward(x)) == n * x.
class FFT {
public:
    void init(int n) {
        if (n == n_) return;
        n_ = n;
        rev_.assign((size_t)n, 0);
        for (int i = 0, j = 0; i < n; ++i) {
            rev_[(size_t)i] = j;
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j |= bit;
        }
        cos_.resize((size_t)n / 2);
        sin_.resize((size_t)n / 2);
        for (int k = 0; k < n / 2; ++k) {
            cos_[(size_t)k] = (float)std::cos(kTwoPi * k / n);
            sin_[(size_t)k] = (float)std::sin(kTwoPi * k / n);
        }
    }
    int size() const { return n_; }
    void forward(float* re, float* im) const { run(re, im, -1.f); }
    void inverse(float* re, float* im) const { run(re, im, 1.f); }

private:
    void run(float* re, float* im, float sign) const {
        const int n = n_;
        for (int i = 0; i < n; ++i) {
            int j = rev_[(size_t)i];
            if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        for (int len = 2; len <= n; len <<= 1) {
            int half = len >> 1, step = n / len;
            for (int i = 0; i < n; i += len)
                for (int k = 0; k < half; ++k) {
                    float wr = cos_[(size_t)(k * step)], wi = sign * sin_[(size_t)(k * step)];
                    float* ar = re + i + k; float* ai = im + i + k;
                    float* br = re + i + k + half; float* bi = im + i + k + half;
                    float tr = *br * wr - *bi * wi, ti = *br * wi + *bi * wr;
                    *br = *ar - tr; *bi = *ai - ti;
                    *ar += tr; *ai += ti;
                }
        }
    }
    int n_ = 0;
    std::vector<int> rev_;
    std::vector<float> cos_, sin_;
};

// wraps a phase into [-pi, pi]
inline float wrapPhase(float p) {
    if (p >= -(float)kPi && p <= (float)kPi) return p;
    return p - (float)kTwoPi * std::floor((p + (float)kPi) / (float)kTwoPi);
}

}  // namespace dsp
