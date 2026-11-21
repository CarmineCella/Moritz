// signals.h — offline signal processing on vectors, C++ half. The Musil half
// is signals.mu. Ported from Musil 1.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// A signal is a vector of samples. A complex spectrum is (list re im): two
// vectors of the same length. A polar spectrum is (list mag phase). A stereo
// buffer is (list left right), as read-wav returns it.
//
// Only what needs an element loop is here: the FFT, the table oscillator,
// the recursive (IIR) filter, fractional delay, windowed-sinc resampling,
// autocorrelation, channel (de)interleaving, peak picking, gather, the in-place
// overlap-add, and the comb and allpass delay lines; and the engines Moritz's
// objects share (signals/*.h), among them the analyses of mir.h (the low-level
// descriptors and YIN's f0 of descriptors~ too). Windows, wavetables, polar
// conversion, filter design, comb and allpass sections, STFT and overlap-add
// are compositions of vector operations and live in signals.mu.

#pragma once
#include "core.h"
#include "signals/fft.h"
#include "signals/vocoder.h"
#include "signals/nebula.h"
#include "signals/psola.h"
#include "signals/ambisonics.h"
#include "signals/hrtf.h"
#include "signals/convolution.h"
#include "signals/filters.h"
#include "signals/mir.h"
#include "signals/hpss.h"
#include "signals/biquad.h"
#include "signals/iir.h"
#include "signals/morphology.h"
#include "signals/envelope.h"
#include "signals/denormal.h"
#include "signals/edit.h"
#include <map>
#include <memory>
#include <mutex>

namespace musil {

using dsp::next_pow2;
using dsp::fft_inplace;      // signals/fft.h

// (fft signal) => (list re im), each of length next-pow2(length); the signal is zero-padded
inline vptr sig_fft(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); size_t n = next_pow2(std::max<size_t>(2, x.size()));
    std::vector<double> buf(2 * n, 0.0);
    for (size_t k = 0; k < x.size(); k++) buf[2 * k] = x[k];
    fft_inplace(buf.data(), n, -1);
    varr re(n), im(n);
    for (size_t k = 0; k < n; k++) { re[k] = buf[2 * k]; im[k] = buf[2 * k + 1]; }
    return v_list({ v_arr(std::move(re)), v_arr(std::move(im)) });
}
// (ifft (list re im)) => real signal of the same length (a power of two)
inline vptr sig_ifft(vlist& a, Interp& i) {
    vlist& s = i.list(a[0]);
    if (s.size() != 2) i.bad("a spectrum is (list re im)");
    const varr& re = i.num(s[0]); const varr& im = i.num(s[1]);
    size_t n = re.size();
    if (im.size() != n) i.bad("re and im must have the same length");
    if (n < 2 || (n & (n - 1))) i.bad("spectrum length must be a power of two (as fft produces)");
    std::vector<double> buf(2 * n);
    for (size_t k = 0; k < n; k++) { buf[2 * k] = re[k]; buf[2 * k + 1] = im[k]; }
    fft_inplace(buf.data(), n, 1);
    varr out(n); for (size_t k = 0; k < n; k++) out[k] = buf[2 * k] / (double)n;
    return v_arr(std::move(out));
}

// (osc sr freqs table) => one sample per element of freqs, reading the wavetable with linear
// interpolation. The table's last element must equal its first (a guard point; gen makes one).
// --- stateful loops: each is written once, with its state as an argument, so the offline builtin
// (fresh state, the whole vector) and the streaming node in live.h (state kept between blocks)
// run the same code and produce the same samples ---
struct osc_state { double phase = 0; };
inline void osc_run(const double* freq, double* out, size_t n, const double* table, size_t table_len, double sr, osc_state& st) {
    size_t len = table_len - 1; double fn = sr / (double)len;
    for (size_t k = 0; k < n; k++) {
        size_t ip = (size_t)st.phase; double fp = st.phase - (double)ip;
        out[k] = (1 - fp) * table[ip] + fp * table[ip + 1];
        st.phase += freq[k] / fn;
        while (st.phase >= (double)len) st.phase -= (double)len;
        while (st.phase < 0) st.phase += (double)len;
    }
}
inline vptr sig_osc(vlist& a, Interp& i) {
    double sr = i.scalar(a[0]); const varr& f = i.num(a[1]); const varr& t = i.num(a[2]);
    if (t.size() < 2) i.bad("table must have at least 2 elements");
    varr out(f.size()); osc_state st;
    if (f.size()) osc_run(&f[0], &out[0], f.size(), &t[0], t.size(), sr, st);
    return v_arr(std::move(out));
}

// (iir x b a) => y with a0 y[n] = sum b[k] x[n-k] - sum a[k>0] y[n-k]; a[0] need not be 1
// iir_state and iir_run are in signals/iir.h
inline vptr sig_iir(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); const varr& b = i.num(a[1]); const varr& av = i.num(a[2]);
    if (av.size() == 0 || b.size() == 0) i.bad("b and a must not be empty");
    if (av[0] == 0) i.bad("a[0] must be nonzero");
    varr y(0.0, x.size()); iir_state st;
    if (x.size()) iir_run(&x[0], &y[0], x.size(), &b[0], b.size(), &av[0], av.size(), st);
    return v_arr(std::move(y));
}

// (delay x d) => x delayed by d samples (fractional, linear interpolation), same length
struct delay_state { std::vector<double> ring; size_t w = 0; };   // a ring of past inputs; w is the write index
inline void delay_run(const double* x, double* y, size_t n, const double* d, delay_state& st, size_t max_delay) {
    size_t cap = max_delay + 2;
    if (st.ring.size() != cap) { st.ring.assign(cap, 0.0); st.w = 0; }
    for (size_t k = 0; k < n; k++) {
        st.ring[st.w] = x[k];
        double dd = d[k] < 0 ? 0 : (d[k] > (double)max_delay ? (double)max_delay : d[k]);
        double pos = (double)st.w - dd; while (pos < 0) pos += cap;
        size_t i0 = (size_t)pos, i1 = (i0 + 1) % cap; double fr = pos - (double)i0;
        y[k] = (1 - fr) * st.ring[i0] + fr * st.ring[i1 % cap];
        st.w = (st.w + 1) % cap;
    }
}
inline vptr sig_delay(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); double d = i.scalar(a[1]);
    if (d < 0) i.bad("delay must be >= 0");
    size_t n = x.size(); varr y(0.0, n); delay_state st; varr dd(d, n);
    if (n) delay_run(&x[0], &y[0], n, &dd[0], st, (size_t)std::ceil(d) + 1);
    return v_arr(std::move(y));
}

// (resample x factor) => x with its length multiplied by factor, by windowed-sinc interpolation
// (any ratio). Upsampling (factor >= 1): a Hann-windowed sinc of 24 zero crossings a side at the input
// Nyquist. Downsampling: the sinc is also the anti-aliasing lowpass, a Kaiser-windowed sinc (beta 7, about
// -70 dB in the stopband) with its cutoff at 0.93 of the new Nyquist and 32 of its zero crossings a side
// (a half-width of 32 / factor input samples), so the transition band ends at the new Nyquist whatever the
// factor: flat to about 0.86 of the new Nyquist, aliases below -65 dB.
struct resample_table { double fc = -1; int half = 0, taps = 0, Q = 0; std::vector<double> t; };
inline double bessel_i0(double x) { double s = 1, term = 1; for (int k = 1; k < 64; k++) { term *= (x / (2 * k)) * (x / (2 * k)); s += term; if (term < 1e-17 * s) break; } return s; }
// the windowed sinc tabulated over Q fractional phases (interpolated between them), each phase normalised so that
// a constant stays a constant; built once per cutoff and thread (a note's pitch shift asks for thousands)
inline const resample_table& resample_kernel(double factor) {
    thread_local resample_table tab;
    double fc = factor < 1 ? factor : 1.0;                 // the kernel's scale, relative to the input Nyquist
    if (tab.fc == fc) return tab;
    const double pi = 3.14159265358979323846;
    bool down = fc < 1;
    // 32 zero crossings of the lowpass each side, at most 4096 taps each side (a factor under 1/128: a kernel that
    // long would take gigabytes; a shorter one still cuts at fc, with a wider transition band)
    int half = down ? (int)std::min(4096.0, std::ceil(32.0 / fc)) : 24, taps = 2 * half;
    int Q = down ? std::max(64, (int)std::lround(512 * fc)) : 512;     // a wider kernel is smoother: fewer phases
    double cut = down ? 0.93 * fc : 1.0, beta = 7.0, i0b = bessel_i0(beta);
    tab.t.assign((size_t)(Q + 1) * taps, 0.0);
    for (int q = 0; q <= Q; q++) {
        double phase = (double)q / Q, wsum = 0; double* row = &tab.t[(size_t)q * taps];
        for (int m = -half + 1; m <= half; m++) {
            double d = phase - m, win;
            if (down) { double r = d / half; win = std::fabs(r) >= 1 ? 0.0 : bessel_i0(beta * std::sqrt(1 - r * r)) / i0b; }
            else win = 0.5 + 0.5 * std::cos(pi * d / half);
            double sn = d == 0 ? cut : std::sin(pi * cut * d) / (pi * d);
            row[m + half - 1] = sn * win; wsum += sn * win;
        }
        if (wsum != 0) for (int m = 0; m < taps; m++) row[m] /= wsum;
    }
    tab.fc = fc; tab.half = half; tab.taps = taps; tab.Q = Q;
    return tab;
}
inline varr resample_sinc(const varr& x, double factor) {
    size_t in = x.size(); if (in == 0) return varr();
    size_t out_len = std::max<size_t>(1, (size_t)std::floor(in * factor + 0.5));
    const resample_table& K = resample_kernel(factor);
    const int half = K.half, taps = K.taps, Q = K.Q;
    varr out(0.0, out_len);
    for (size_t k = 0; k < out_len; k++) {
        double t = (double)k / factor; long c = (long)std::floor(t); double phase = t - (double)c;
        double qf = phase * Q; int q = (int)qf; double u = qf - q; if (q >= Q) { q = Q - 1; u = 1; }
        const double* r0 = &K.t[(size_t)q * taps]; const double* r1 = r0 + taps;
        double acc = 0;
        long j0 = c - half + 1, j1 = c + half;
        long lo = std::max(j0, 0L), hi = std::min(j1, (long)in - 1);
        for (long j = lo; j <= hi; j++) { int m = (int)(j - j0); acc += x[(size_t)j] * ((1 - u) * r0[m] + u * r1[m]); }
        out[k] = acc;
    }
    return out;
}
inline vptr sig_resample(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); double factor = i.scalar(a[1]);
    if (factor <= 0) i.bad("factor must be > 0");
    return v_arr(resample_sinc(x, factor));
}

// (autocorr x) => biased autocorrelation (sum / n) for lags 0 .. n/2 - 1; it decays with lag, which
// makes the first periodicity peak the largest. By FFT: |X|^2 of the signal padded to twice its length
inline std::vector<double> autocorr_fft(const double* x, size_t n, size_t lags) {
    size_t N = dsp::next_pow2(std::max<size_t>(2, 2 * n));
    std::vector<double> buf(N, 0.0), re(N / 2 + 1), im(N / 2 + 1);
    std::copy(x, x + n, buf.begin());
    auto& f = dsp::rfft_plan<double>((int)N);
    f.forward(buf.data(), re.data(), im.data());
    for (size_t k = 0; k <= N / 2; k++) { re[k] = re[k] * re[k] + im[k] * im[k]; im[k] = 0; }
    f.inverse(re.data(), im.data(), buf.data());
    std::vector<double> r(lags, 0.0);
    for (size_t lag = 0; lag < lags && lag < N; lag++) r[lag] = buf[lag] / (double)N / (double)n;
    return r;
}
inline vptr sig_autocorr(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); size_t n = x.size();
    if (n < 4) i.bad("signal too short");
    auto r = autocorr_fft(&x[0], n, n / 2);
    return v_arr(varr(r.data(), r.size()));
}

// (interleave (list a b ...)) => one vector a0 b0 a1 b1 ...
inline vptr sig_interleave(vlist& a, Interp& i) {
    vlist& ch = i.list(a[0]); if (ch.empty()) i.bad("no channels");
    size_t n = i.num(ch[0]).size(), c = ch.size();
    for (auto& v : ch) if (i.num(v).size() != n) i.bad("all channels must have the same length");
    varr out(n * c);
    for (size_t k = 0; k < n; k++) for (size_t j = 0; j < c; j++) out[k * c + j] = ch[j]->num[k];
    return v_arr(std::move(out));
}
// (deinterleave v channels) => (list a b ...): the channels of an interleaved vector
inline vptr sig_deinterleave(vlist& a, Interp& i) {
    const varr& v = i.num(a[0]); long c = i.index(a[1]);
    if (c < 1) i.bad("channels must be >= 1");
    if (v.size() % (size_t)c) i.bad("length is not a multiple of the channel count");
    size_t n = v.size() / (size_t)c; vlist out;
    for (long j = 0; j < c; j++) { varr ch(n); for (size_t k = 0; k < n; k++) ch[k] = v[k * c + j]; out.push_back(v_arr(std::move(ch))); }
    return v_list(std::move(out));
}

// (local-maxima v) => indices k with v[k] > v[k-1] and v[k] > v[k+1] (interior points only)
inline vptr sig_local_maxima(vlist& a, Interp& i) {
    const varr& v = i.num(a[0]); std::vector<double> out;
    for (size_t k = 1; k + 1 < v.size(); k++) if (v[k] > v[k - 1] && v[k] > v[k + 1]) out.push_back((double)k);
    return from_vec(out);
}
// (gather v indices) => the elements of v at the given (integer) indices, as a vector
inline vptr sig_gather(vlist& a, Interp& i) {
    const varr& v = i.num(a[0]); const varr& idx = i.num(a[1]); varr out(idx.size());
    for (size_t k = 0; k < idx.size(); k++) {
        long j = (long)idx[k]; if (j < 0) j += (long)v.size();
        if (j < 0 || (size_t)j >= v.size()) i.bad("index " + std::to_string((long)idx[k]) + " out of range (size " + std::to_string(v.size()) + ")");
        out[k] = v[j];
    }
    return v_arr(std::move(out));
}

// (add-at! dst pos src) => dst, with src added in place starting at pos; dst must be long enough.
// The overlap-add of an STFT is O(total) with this and O(frames x total) with copies.
inline vptr sig_add_at_inplace(vlist& a, Interp& i) {
    varr& dst = a[0]->t == Value::NUM ? a[0]->num : (i.num(a[0]), a[0]->num);
    long pos = i.index(a[1]); const varr& src = i.num(a[2]);
    if (pos < 0 || (size_t)pos + src.size() > dst.size()) i.bad("does not fit: " + std::to_string(src.size()) + " samples at " + std::to_string(pos) + " into " + std::to_string(dst.size()));
    for (size_t k = 0; k < src.size(); k++) dst[pos + k] += src[k];
    return a[0];
}
// (comb x d g) => feedback comb y[n] = x[n] + g y[n-d]; (allpass x d g) => Schroeder allpass
// y[n] = -g x[n] + x[n-d] + g y[n-d]. Both are a delay line, not a dense IIR: O(n) whatever d is.
struct comb_state { std::vector<double> ring; size_t w = 0; };
// y[n] = x[n] + g y[n-d]
inline void comb_run(const double* x, double* y, size_t n, size_t d, double g, comb_state& st) {
    dsp::DenormalGuard ftz;                                // the decaying tail: no denormals (x86)
    if (st.ring.size() != d) { st.ring.assign(d, 0.0); st.w = 0; }
    for (size_t k = 0; k < n; k++) { double out = x[k] + g * st.ring[st.w]; st.ring[st.w] = out; st.w = (st.w + 1) % d; y[k] = out; }
}
struct allpass_state { std::vector<double> xin, yout; size_t w = 0; };
// y[n] = -g x[n] + x[n-d] + g y[n-d]
inline void allpass_run(const double* x, double* y, size_t n, size_t d, double g, allpass_state& st) {
    dsp::DenormalGuard ftz;                                // the decaying tail: no denormals (x86)
    if (st.xin.size() != d) { st.xin.assign(d, 0.0); st.yout.assign(d, 0.0); st.w = 0; }
    for (size_t k = 0; k < n; k++) { double out = -g * x[k] + st.xin[st.w] + g * st.yout[st.w]; st.xin[st.w] = x[k]; st.yout[st.w] = out; st.w = (st.w + 1) % d; y[k] = out; }
}
inline vptr sig_comb(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); long d = i.index(a[1]); double g = i.scalar(a[2]);
    if (d < 1) i.bad("delay must be >= 1");
    varr y(0.0, x.size()); comb_state st;
    if (x.size()) comb_run(&x[0], &y[0], x.size(), (size_t)d, g, st);
    return v_arr(std::move(y));
}
inline vptr sig_allpass(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); long d = i.index(a[1]); double g = i.scalar(a[2]);
    if (d < 1) i.bad("delay must be >= 1");
    varr y(0.0, x.size()); allpass_state st;
    if (x.size()) allpass_run(&x[0], &y[0], x.size(), (size_t)d, g, st);
    return v_arr(std::move(y));
}

// --- the phase vocoder ---------------------------------------------------------------
// (pvoc x opts): sparkle's phase vocoder (signals/vocoder.h, the engine of Moritz's sparkle~) over
// a whole sound (dsp::pvoc, which the sound editor of Moritz runs too). Parameters may ramp: a value
// given as (list start end).
struct pvoc_opts : dsp::PvocOptions { std::string phase; const varr* other = nullptr; };
inline void pvoc_ramp(Interp& i, const vptr& v, double& a, double& b) {
    if (v->t == Value::LIST) { if (v->l.size() != 2) i.bad("a ramp is (list start end)"); a = i.scalar(v->l[0]); b = i.scalar(v->l[1]); }
    else a = b = i.scalar(v);
}
inline vptr sig_pvoc(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); vlist& opts = i.list(a[1]);
    pvoc_opts o;
    for (auto& e : opts) {
        vlist& kv = i.list(e); if (kv.size() != 2 || kv[0]->t != Value::STR) i.bad("an option is (list \"key\" value)");
        const std::string& k = kv[0]->s; const vptr& v = kv[1];
        if (k == "stretch") pvoc_ramp(i, v, o.stretch0, o.stretch1);
        else if (k == "pitch") pvoc_ramp(i, v, o.pitch0, o.pitch1);
        else if (k == "formants") pvoc_ramp(i, v, o.form0, o.form1);
        else if (k == "threshold") pvoc_ramp(i, v, o.thr0, o.thr1);
        else if (k == "window") o.window = (int)i.scalar(v);
        else if (k == "overlap") o.overlap = (int)i.scalar(v);
        else if (k == "pad") o.pad = (int)i.scalar(v);
        else if (k == "envelope") o.env = (int)i.scalar(v);
        else if (k == "phase") o.phase = i.str(v);
        else if (k == "cross") {
            vlist& c = i.list(v); if (c.size() != 3 && c.size() != 4) i.bad("cross is (list mode amount other) or (list mode start end other)");
            o.xmode = (int)i.scalar(c[0]); o.xa0 = i.scalar(c[1]); o.xa1 = c.size() == 4 ? i.scalar(c[2]) : o.xa0; o.other = &i.num(c.back());
            if (o.xmode < 1 || o.xmode > 3) i.bad("cross mode is 1, 2 or 3");
        } else i.bad("unknown option " + k);
    }
    if (o.window < 8 || o.overlap < 1 || o.pad < 0 || o.env < 0 || o.stretch0 <= 0 || o.stretch1 <= 0 || o.pitch0 <= 0 || o.pitch1 <= 0 || o.form0 <= 0 || o.form1 <= 0) i.bad("invalid parameters");
    if (o.xmode == 2 && o.env == 0) i.bad("cross mode 2 needs an envelope order");
    if (!o.phase.empty() && o.phase != "robot" && o.phase != "whisper") i.bad("phase is \"robot\" or \"whisper\"");
    size_t tot = x.size(); if (tot == 0) return v_arr(varr());
    if (o.other && o.other->size() == 0) i.bad("cross: the other sound is empty");
    o.phaseMode = o.phase == "robot" ? 1 : o.phase == "whisper" ? 2 : 0;
    const double* other = o.other ? &(*o.other)[0] : nullptr;
    std::vector<double> y = dsp::pvoc(&x[0], tot, o, other, o.other ? o.other->size() : 0, [&i] { return (uint32_t)i.rng(); });
    return v_arr(varr(y.data(), y.size()));
}
// (deconv y h [threshold]) => x such that (conv x h) ~ y, as long as y: the spectrum of y divided by that
//   of h, regularised (Tikhonov): Y conj(H) / (|H|^2 + threshold^2 max|H|^2), threshold 0.001 by default, so
//   where |H| is far above threshold times its peak it is Y / H, at that level half of it, and below it the
//   gain falls smoothly to 0 instead of being cut; threshold 0 is the plain inverse (signals/convolution.h, as
//   Moritz's condec): a recording un-reverberated by a measured response, or a response measured from a known sweep
inline vptr sig_deconv(vlist& a, Interp& i) {
    const varr& y = i.num(a[0]); const varr& h = i.num(a[1]); double thr = a.size() > 2 ? i.scalar(a[2]) : 0.001;
    if (thr < 0) i.bad("threshold must be >= 0");
    std::vector<double> ys(std::begin(y), std::end(y)), hs(std::begin(h), std::end(h));
    auto x = dsp::deconvolve(ys, hs, thr);
    return v_arr(varr(x.data(), x.size()));
}

// (spectral-envelope mags order)   smooth envelope of a full magnitude spectrum (all n bins, n a power of two, as
//   magnitudes of an fft gives): the "true envelope", a cepstral smoothing (the first `order` coefficients) iterated
//   12 times so that it rides on the harmonic peaks instead of averaging peaks and valleys (signals/envelope.h, the
//   estimator of Moritz's specenv~). The order sets the resolution: about sr / (2 f0) of the sound, f0 its lowest
//   fundamental (150 for a voice at 44.1 kHz), follows the formants without rippling at the harmonics; lower is smoother
inline vptr sig_spectral_envelope(vlist& a, Interp& i) {
    const varr& m = i.num(a[0]); long order = i.index(a[1]); size_t n = m.size();
    if (n < 4 || (n & (n - 1))) i.bad("the spectrum's length must be a power of two >= 4 (as fft gives)");
    if (order < 0) i.bad("order must be >= 0");
    std::vector<double> mags(std::begin(m), std::end(m)), env(n);
    thread_local dsp::TrueEnvelope<double> te; te.init((int)n);
    te.full(mags.data(), (int)order, 12, env.data());
    return v_arr(varr(env.data(), n));
}

// (biquad type sr f0 q gain-db) => (list b a) of an RBJ biquad (the cookbook's coefficients, a[0] not 1: iir takes
//   them as they are); type is one of "lowpass" "highpass" "bandpass" "notch" "peak" "lowshelf" "highshelf" "allpass"; gain-db
//   is for peak and the shelves (signals/biquad.h, the coefficients of Moritz's biquad objects and of mir.h too)
inline vptr sig_biquad(vlist& a, Interp& i) {
    for (size_t k = 2; k < 5; ++k) if (a[k]->t == Value::NUM && a[k]->num.size() > 1) i.bad("biquad: a moving cutoff, q or gain is a streaming feature (a synth); offline they are single numbers");
    dsp::BiquadType t; std::string name = i.str(a[0]);
    if (!dsp::biquad_type(name, t)) i.bad("biquad: unknown type " + name);
    auto c = dsp::rbj(t, i.scalar(a[1]), i.scalar(a[2]), i.scalar(a[3]), i.scalar(a[4]));
    varr b(3), av(3); b[0] = c.b0; b[1] = c.b1; b[2] = c.b2; av[0] = c.a0; av[1] = c.a1; av[2] = c.a2;
    return v_list({ v_arr(std::move(b)), v_arr(std::move(av)) });
}

// (svf x sr freq q type) => x through a state-variable filter (signals/filters.h, as Moritz's svf~);
//   type is "low" "high" "band" or "notch"; q in 0..1 is the resonance; freq and q are numbers or vectors
//   as long as x (a moving filter). Streams in a synth (freq and q read once per block there). The band
//   output is not normalised (as svf~): its peak gain is 1/(2 - 1.98 q), -6 dB at q 0, 0 dB at 0.5, +34 dB at 1
inline int svf_type(Interp& i, const vptr& v) {
    std::string t = i.str(v);
    if (t == "low") return 0; if (t == "high") return 1; if (t == "band") return 2; if (t == "notch") return 3;
    i.bad("type is \"low\" \"high\" \"band\" or \"notch\""); return 0;
}
inline vptr sig_svf(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); double sr = i.scalar(a[1]); const varr& f = i.num(a[2]); const varr& q = i.num(a[3]); int type = svf_type(i, a[4]);
    if (sr <= 0) i.bad("sr must be > 0");
    if ((f.size() != 1 && f.size() != x.size()) || (q.size() != 1 && q.size() != x.size())) i.bad("freq and q are numbers or vectors as long as x");
    dsp::Svf flt; varr y(0.0, x.size()); dsp::DenormalGuard ftz;
    bool moving = f.size() > 1 || q.size() > 1;
    if (!moving && f.size() && q.size()) flt.set(f[0], q[0], sr);
    for (size_t k = 0; k < x.size(); k++) {
        if (moving) flt.set(f.size() > 1 ? f[k] : f[0], q.size() > 1 ? q[k] : q[0], sr);
        double o[4]; flt.tick(x[k], o[0], o[1], o[2], o[3]); y[k] = o[type];
    }
    return v_arr(std::move(y));
}

// --- nebula -------------------------------------------------------------------------
// (nebula sr seconds opts) => (list left right): a cloud of grains (signals/nebula.h, the engine of
// Moritz's nebula~), spawned for `seconds`, then left to finish. opts is a record, (list key value)
// pairs: "proto1" "proto2" a waveform ("sine" "triangle" "square" "noise") or a sound (a vector at
// sr); "pitch1" "pitch2" a sound's pitch in Hz (then freq is in Hz for it too); "density" grains per
// second, "length" ms, "freq" Hz (or a speed ratio for a sound), "morph" 0..1, "amp", "gliss" 0..1,
// "jitter" pan 0 right .. 1 left, "speed", "grainlets" periods per grain: each a number or a ramp
// (list start end); "density-random" "length-random" "freq-random" "jitter-random" ratios of the
// value; "window" "hann" "hamming" "blackman"; "poly" at most this many grains; "seed".
inline vptr sig_nebula(vlist& a, Interp& i) {
    double sr = i.scalar(a[0]), secs = i.scalar(a[1]); vlist& opts = i.list(a[2]);
    if (sr <= 0 || secs < 0) i.bad("sr must be > 0 and seconds >= 0");
    using Neb = dsp::Nebula<double>;
    auto neb = std::make_unique<Neb>();
    struct Ramp { double a, b; double* to; };
    std::vector<Ramp> ramps;
    auto& p = neb->p;
    std::string proto[2] = { "sine", "" };
    const varr* sound[2] = { nullptr, nullptr };
    std::string window = "hann";
    bool seeded = false;
    for (auto& e : opts) {
        vlist& kv = i.list(e); if (kv.size() != 2 || kv[0]->t != Value::STR) i.bad("an option is (list \"key\" value)");
        const std::string& k = kv[0]->s; const vptr& v = kv[1];
        auto ramp = [&](double& to) { double x0, x1; pvoc_ramp(i, v, x0, x1); to = x0; ramps.push_back({ x0, x1, &to }); };
        if (k == "proto1" || k == "proto2") {
            int n = k == "proto1" ? 0 : 1;
            if (v->t == Value::STR) { if (!Neb::isWave(v->s)) i.bad("a waveform is \"sine\" \"triangle\" \"square\" or \"noise\""); proto[n] = v->s; sound[n] = nullptr; }
            else { sound[n] = &i.num(v); proto[n].clear(); }
        }
        else if (k == "pitch1") p.pitch[0] = i.scalar(v);
        else if (k == "pitch2") p.pitch[1] = i.scalar(v);
        else if (k == "density") ramp(p.density);
        else if (k == "length") ramp(p.length);
        else if (k == "freq") ramp(p.freq);
        else if (k == "morph") ramp(p.morph);
        else if (k == "amp") ramp(p.amp);
        else if (k == "gliss") ramp(p.gliss);
        else if (k == "jitter") ramp(p.jitter);
        else if (k == "speed") ramp(p.speed);
        else if (k == "grainlets") ramp(p.grainlets);
        else if (k == "density-random") p.densityRnd = i.scalar(v);
        else if (k == "length-random") p.lengthRnd = i.scalar(v);
        else if (k == "freq-random") p.freqRnd = i.scalar(v);
        else if (k == "jitter-random") p.jitterRnd = i.scalar(v);
        else if (k == "window") window = i.str(v);
        else if (k == "poly") p.poly = (int)i.scalar(v);
        else if (k == "seed") { neb->seed((unsigned)i.scalar(v)); seeded = true; }
        else i.bad("unknown option " + k);
    }
    if (window != "hann" && window != "hamming" && window != "blackman") i.bad("window is \"hann\" \"hamming\" or \"blackman\"");
    if (proto[1].empty() && !sound[1]) { proto[1] = proto[0]; sound[1] = sound[0]; }      // proto2 is proto1 unless given
    for (int n = 0; n < 2; n++) {
        if (sound[n]) neb->setSound(n, sound[n]->size() ? &(*sound[n])[0] : nullptr, (long)sound[n]->size(), 1, sr);
        else neb->setWave(n, proto[n]);
    }
    neb->setWindow(window);
    if (!seeded) neb->seed((unsigned)(i.rng() % 2147483646u) + 1u);
    const int B = 64;
    size_t total = (size_t)std::llround(secs * sr), cap = total + (size_t)(10 * sr);
    std::vector<double> L, R; L.reserve(total); R.reserve(total);
    double l[B], r[B];
    for (size_t t = 0; t < cap; t += B) {
        p.on = t < total;
        if (!p.on && neb->active() == 0) break;
        double x = total > 0 ? std::min(1.0, (double)t / (double)total) : 0;
        for (auto& rp : ramps) *rp.to = rp.a + (rp.b - rp.a) * x;
        int n = (int)std::min<size_t>(B, std::max(cap - t, (size_t)0));
        neb->process(l, r, n, sr);
        for (int k = 0; k < n; k++) { L.push_back(l[k]); R.push_back(r[k]); }
    }
    if (L.size() < total) { L.resize(total, 0.0); R.resize(total, 0.0); }
    return v_list({ v_arr(varr(L.data(), L.size())), v_arr(varr(R.data(), R.size())) });
}

// --- harmonize ----------------------------------------------------------------------
// numbers from a number, a vector or a list of numbers
inline std::vector<double> harm_numbers(Interp& i, const vptr& v) {
    std::vector<double> out;
    if (v->t == Value::LIST) for (auto& e : v->l) out.push_back(i.scalar(e));
    else { const varr& x = i.num(v); out.assign(std::begin(x), std::end(x)); }
    return out;
}
inline std::string harm_name(Interp& i, const vptr& v) {
    if (v->t == Value::STR || v->t == Value::SYM) return v->s;
    i.bad("expected a name"); return "";
}
struct HarmNote { size_t on, off; int pitch; double vel; };
struct HarmRun {
    std::unique_ptr<dsp::Psola<double>> ps;
    std::vector<HarmNote> notes;
    long latency = 0;
};
// the harmonizer of x set up by the arguments of harmonize: (x sr voices opts)
inline HarmRun harm_setup(vlist& a, Interp& i, size_t nvoices_at, size_t opts_at) {
    using Ps = dsp::Psola<double>;
    HarmRun run;
    run.ps = std::make_unique<Ps>();
    Ps& ps = *run.ps;
    double sr = i.scalar(a[1]);
    if (!std::isfinite(sr) || sr < 8000 || sr > 384000) i.bad("sr must be in 8000 .. 384000");
    std::vector<double> voices;
    if (nvoices_at < a.size()) voices = harm_numbers(i, a[nvoices_at]);
    else voices = {0};
    if (voices.empty() || voices.size() > (size_t)Ps::kMaxVoices) i.bad("from 1 to 8 voices");
    auto& p = ps.p;
    p.voices = (int)voices.size();
    for (size_t k = 0; k < voices.size(); ++k) {
        p.v[k] = Ps::Voice{};
        p.v[k].interval = voices[k];
        p.v[k].pan = voices.size() > 1 ? -0.7 + 1.4 * (double)k / (double)(voices.size() - 1) : 0.0;
    }
    Ps::Setup st; st.sr = sr;
    std::string window = "hann";
    bool seeded = false;
    uint32_t seed = 1;
    int key = 0;
    const int* steps = nullptr; int nsteps = 0; std::vector<int> own;
    const music::ScaleDef* major = music::scale_named("major");
    steps = major->steps; nsteps = major->n;
    auto perVoice = [&](const vptr& v, double Ps::Voice::*field) {
        auto x = harm_numbers(i, v);
        if (x.empty()) i.bad("a value for each voice, or one for all");
        for (size_t k = 0; k < voices.size(); ++k) p.v[k].*field = x[std::min(k, x.size() - 1)];
    };
    if (opts_at < a.size())
        for (auto& e : i.list(a[opts_at])) {
            vlist& kv = i.list(e);
            if (kv.size() != 2) i.bad("an option is (list \"key\" value)");
            std::string k = harm_name(i, kv[0]);
            const vptr& v = kv[1];
            if (k == "mode") { if (!dsp::harmony_mode(harm_name(i, v), p.mode)) i.bad("mode is \"intervals\" \"scale\" \"midi-chord\" or \"midi-root\""); }
            else if (k == "key") {
                if (v->t == Value::NUM) key = (((int)std::lround(i.scalar(v)) % 12) + 12) % 12;
                else { key = music::key_class(harm_name(i, v)); if (key < 0) i.bad("a key is 0..11 or a name: C, F#, Bb ..."); }
            }
            else if (k == "scale") {
                if (v->t == Value::STR || v->t == Value::SYM) {
                    const music::ScaleDef* d = music::scale_named(v->s);
                    if (!d) i.bad("unknown scale " + v->s + " (scale-names lists them)");
                    steps = d->steps; nsteps = d->n;
                } else {
                    own.clear();
                    for (double x : harm_numbers(i, v)) own.push_back((((int)std::lround(x) % 12) + 12) % 12);
                    if (own.empty()) i.bad("a scale is a name or a list of pitch classes");
                    steps = own.data(); nsteps = (int)own.size();
                }
            }
            else if (k == "notes") {
                for (auto& nv : i.list(v)) {
                    auto x = harm_numbers(i, nv);
                    if (x.size() < 3) i.bad("a note is (list onset duration pitch [velocity]), in seconds");
                    if (x[0] < 0 || x[1] < 0) i.bad("a note's onset and duration are >= 0");
                    double vel = x.size() > 3 ? x[3] : 100;
                    size_t on = (size_t)std::llround(x[0] * sr), off = (size_t)std::llround((x[0] + x[1]) * sr);
                    run.notes.push_back({on, std::max(on + 1, off), (int)std::lround(x[2]), vel});
                }
            }
            else if (k == "levels") perVoice(v, &Ps::Voice::level);
            else if (k == "pans") perVoice(v, &Ps::Voice::pan);
            else if (k == "formants") perVoice(v, &Ps::Voice::formant);
            else if (k == "vibrato") {
                auto x = harm_numbers(i, v);
                if (x.size() < 2) i.bad("vibrato is (list depth-cents rate-hz [random])");
                for (size_t n = 0; n < voices.size(); ++n) { p.v[n].vibDepth = x[0]; p.v[n].vibRate = x[1]; p.v[n].vibRandom = x.size() > 2 ? x[2] : 0; }
            }
            else if (k == "humanize") {
                auto x = harm_numbers(i, v);
                if (x.size() != 2) i.bad("humanize is (list cents ms)");
                p.humanizeCents = std::max(0.0, x[0]); p.humanizeMs = std::max(0.0, x[1]);
            }
            else if (k == "transpose") p.transpose = i.scalar(v);
            else if (k == "dry") p.dry = std::max(0.0, i.scalar(v));
            else if (k == "wet") p.wet = std::max(0.0, i.scalar(v));
            else if (k == "fmin") st.fmin = i.scalar(v);
            else if (k == "fmax") st.fmax = i.scalar(v);
            else if (k == "threshold") { st.threshold = i.scalar(v); if (!(st.threshold > 0 && st.threshold <= 1)) i.bad("threshold is in (0, 1]"); }
            else if (k == "gate") st.gate = std::pow(10.0, i.scalar(v) / 20.0);
            else if (k == "cycles") p.cycles = v->t == Value::NUM ? std::clamp(i.scalar(v), 0.5, 4.0) : (harm_name(i, v) == "auto" ? 0.0 : (i.bad("cycles is a number or \"auto\""), 0.0));
            else if (k == "window") { window = harm_name(i, v); if (!dsp::grains::GrainWindow::known(window)) i.bad("window is \"hann\" \"hamming\" or \"blackman\""); }
            else if (k == "latency") { st.latencyMs = i.scalar(v); if (!(st.latencyMs >= 0) || st.latencyMs > 500) i.bad("latency is 0 (auto) .. 500 ms"); }
            else if (k == "seed") { seed = (uint32_t)i.scalar(v); seeded = true; }
            else if (k == "marks") { std::string m = harm_name(i, v); if (m != "snap" && m != "period") i.bad("marks is \"snap\" or \"period\""); p.marksPeriod = m == "period"; }
            else if (k == "unvoiced") { std::string m = harm_name(i, v); if (m != "mute" && m != "dry") i.bad("unvoiced is \"mute\" or \"dry\""); p.unvoicedDry = m == "dry"; }
            else if (k == "decimate") st.decimate = (int)i.scalar(v);
            else i.bad("unknown option " + k);
        }
    if (!(st.fmin > 0) || !(st.fmax > st.fmin)) i.bad("need 0 < fmin < fmax");
    ps.init(st);                                      // the parameters stay
    ps.p.scaleMask = music::scale_mask(steps, nsteps, key);
    ps.setWindow(window);
    ps.seed(seeded ? seed : (uint32_t)(i.rng() & 0xffffffffu));
    run.latency = ps.latency();
    return run;
}
// runs x through it: the voices (each as long as x, the latency taken out) into voiceOut[k] when given, the mix into L R,
// the f0 the voices follow into f0 when given
inline void harm_run(Interp& i, HarmRun& run, const varr& x, std::vector<double>* L, std::vector<double>* R,
                     std::vector<std::vector<double>>* voiceOut, std::vector<double>* f0) {
    auto& ps = *run.ps;
    const size_t n = x.size(), D = (size_t)run.latency, total = n + D;
    const int nv = ps.voices();
    std::vector<std::vector<double>> vo(voiceOut ? (size_t)nv : 0, std::vector<double>(total, 0.0));
    std::vector<double> l(L ? total : 0), r(R ? total : 0), fz(f0 ? total : 0);
    struct Ev { size_t at; int pitch; double vel; };
    std::vector<Ev> ev;
    for (auto& nt : run.notes) { ev.push_back({nt.on, nt.pitch, nt.vel}); ev.push_back({nt.off, nt.pitch, 0}); }
    std::stable_sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.at < b.at; });
    size_t e = 0;
    std::vector<double> in(64, 0.0);
    double* ptr[dsp::Psola<double>::kMaxVoices] = {};
    for (size_t t = 0; t < total;) {
        while (e < ev.size() && ev[e].at <= t) {
            if (ev[e].vel > 0) ps.noteOn(ev[e].pitch, ev[e].vel); else ps.noteOff(ev[e].pitch);
            ++e;
        }
        size_t m = std::min<size_t>(64, total - t);
        if (e < ev.size()) m = std::min(m, ev[e].at - t);
        if (m == 0) m = 1;
        for (size_t k = 0; k < m; ++k) in[k] = t + k < n ? x[t + k] : 0.0;
        for (int k = 0; k < nv && voiceOut; ++k) ptr[k] = vo[(size_t)k].data() + t;
        if (f0) {                                    // one sample at a time: the f0 of each
            for (size_t k = 0; k < m; ++k) {
                for (int j = 0; j < nv && voiceOut; ++j) ptr[j] = vo[(size_t)j].data() + t + k;
                ps.process(&in[k], L ? &l[t + k] : nullptr, R ? &r[t + k] : nullptr, 1, voiceOut ? ptr : nullptr);
                fz[t + k] = ps.f0Delayed();
            }
        } else ps.process(in.data(), L ? &l[t] : nullptr, R ? &r[t] : nullptr, (int)m, voiceOut ? ptr : nullptr);
        t += m;
        if ((t & 0xffff) < m) i.yield_check();
    }
    auto cut = [&](std::vector<double>& v) { v.erase(v.begin(), v.begin() + (long)D); };
    if (L) { cut(l); *L = std::move(l); }
    if (R) { cut(r); *R = std::move(r); }
    if (f0) { cut(fz); *f0 = std::move(fz); }
    if (voiceOut) { for (auto& v : vo) cut(v); *voiceOut = std::move(vo); }
}
inline vptr harm_vec(const std::vector<double>& v) { return v_arr(varr(v.data(), v.size())); }
// (harmonize x sr voices opts) => (list left right): the voice x (a vector at sr) with pitch-shifted copies of itself
//   (signals/psola.h, the engine of Moritz's harmonize~): grains of its own periods, overlapped at each copy's pitch,
//   so the formants stay where they are. voices is a list of numbers, one per voice (up to 8): its interval in
//   semitones ("intervals", the default mode), its steps in the scale ("scale": 2 a third, 4 a fifth, -3 a fourth
//   below), its interval above the held note ("midi-root"); in "midi-chord" only their number counts. The output
//   is as long as x and in time with it (the latency is taken out). opts is a record, (list key value) pairs:
//   "mode" "intervals" "scale" "midi-chord" "midi-root"; "key" 0..11 or a name ("Eb"); "scale" a name (scale-names)
//   or a list of pitch classes; "notes" (list (list onset duration pitch [velocity]) ...), seconds, for the midi
//   modes; "levels" "pans" (-1 left .. 1 right) "formants" (1 keeps them, 1.2 a fifth of the way to a child's) a
//   number for all or a list per voice; "vibrato" (list cents hz [random 0..1]); "humanize" (list cents ms): a slow
//   drift and a random onset delay per voice; "transpose" semitones; "dry" the input's level in the mix (0), "wet"
//   (1); "fmin" "fmax" the voice's range (70 1000 Hz); "threshold" YIN's (0.15); "gate" dB under which it is
//   unvoiced (-50); "cycles" input periods per grain or "auto"; "window" "hann" "hamming" "blackman"; "latency"
//   ms (0 auto); "marks" "snap" or "period"; "unvoiced" "mute" or "dry" (the consonants kept in every voice);
//   "seed". Each voice keeps the input's level
inline vptr sig_harmonize(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]);
    HarmRun run = harm_setup(a, i, 2, 3);
    std::vector<double> L, R;
    harm_run(i, run, x, &L, &R, nullptr, nullptr);
    return v_list({ harm_vec(L), harm_vec(R) });
}
// (harmonize-voices x sr voices opts) => a list of vectors, each voice of harmonize alone (its level, not panned)
inline vptr sig_harmonize_voices(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]);
    HarmRun run = harm_setup(a, i, 2, 3);
    std::vector<std::vector<double>> vo;
    harm_run(i, run, x, nullptr, nullptr, &vo, nullptr);
    vlist out;
    for (auto& v : vo) out.push_back(harm_vec(v));
    return v_list(std::move(out));
}
// (harmonize-f0 x sr [opts]) => the f0 harmonize follows, a value per sample of x (Hz, 0 where it is unvoiced): its
//   streaming YIN on the input (a frame every 5 ms), at the time each frame describes; opts as harmonize's ("fmin"
//   "fmax" "threshold" "gate" "latency" ...)
inline vptr sig_harmonize_f0(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]);
    HarmRun run = harm_setup(a, i, 99, 2);
    std::vector<double> f0;
    harm_run(i, run, x, nullptr, nullptr, nullptr, &f0);
    return harm_vec(f0);
}
// (scale-steps name) (scale-names) => the steps of a scale in semitones above its tonic, a list (music/scales.h:
//   the scales of scale-pitches and of harmonize); the names of the scales it knows
inline vptr sig_scale_steps(vlist& a, Interp& i) {
    std::string n = harm_name(i, a[0]);
    const music::ScaleDef* d = music::scale_named(n);
    if (!d) i.bad("unknown scale " + n + " (scale-names lists them)");
    vlist out;
    for (int k = 0; k < d->n; ++k) out.push_back(v_num(d->steps[k]));
    return v_list(std::move(out));
}
inline vptr sig_scale_names(vlist&, Interp&) {
    vlist out;
    for (auto& n : music::scale_names()) out.push_back(v_sym(n));
    return v_list(std::move(out));
}

// (adsr sr gate a d s r) => an envelope following a gate signal (1 = on, 0 = off): attack a and
// decay d seconds towards the sustain level s while the gate is on, release r seconds when it goes off;
// exponential segments, so it sounds even; the same code runs in the live engine's adsr node.
struct adsr_state { double level = 0; int stage = 0; };   // stages: 0 idle, 1 attack, 2 decay/sustain, 3 release
inline void adsr_run(const double* gate, double* out, size_t n, double a, double d, double s, double r, double sr, adsr_state& st) {
    dsp::DenormalGuard ftz;                                // the decaying tail: no denormals (x86)
    auto coef = [&](double secs) { return secs <= 0 ? 0.0 : std::exp(-1.0 / (std::max(secs, 1e-4) * sr)); };
    double ca = coef(a), cd = coef(d), cr = coef(r);
    for (size_t k = 0; k < n; k++) {
        bool on = gate[k] > 0.5;
        if (on && (st.stage == 0 || st.stage == 3)) st.stage = 1;
        if (!on && (st.stage == 1 || st.stage == 2)) st.stage = 3;
        if (st.stage == 1) { st.level = 1.2 - (1.2 - st.level) * ca; if (st.level >= 1) { st.level = 1; st.stage = 2; } }
        else if (st.stage == 2) st.level = s + (st.level - s) * cd;
        else if (st.stage == 3) { st.level *= cr; if (st.level < 1e-5) { st.level = 0; st.stage = 0; } }
        out[k] = st.level;
    }
}
inline vptr sig_adsr(vlist& a, Interp& i) {
    double sr = i.scalar(a[0]); const varr& g = i.num(a[1]);
    double at = i.scalar(a[2]), de = i.scalar(a[3]), su = i.scalar(a[4]), re = i.scalar(a[5]);
    if (sr <= 0 || at < 0 || de < 0 || re < 0 || su < 0 || su > 1) i.bad("times must be >= 0 and sustain in [0, 1]");
    varr out(0.0, g.size()); adsr_state st;
    if (g.size()) adsr_run(&g[0], &out[0], g.size(), at, de, su, re, sr, st);
    return v_arr(std::move(out));
}
// (lag x sr seconds) => x smoothed by a one-pole lowpass with the given time constant (parameter
// smoothing, envelope following); the live engine's lag node
struct lag_state { double y = 0; };
inline void lag_run(const double* x, double* out, size_t n, double coef, lag_state& st) {
    dsp::DenormalGuard ftz;                                // the decaying tail: no denormals (x86)
    for (size_t k = 0; k < n; k++) { st.y += (x[k] - st.y) * (1 - coef); out[k] = st.y; }
}
inline vptr sig_lag(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); double sr = i.scalar(a[1]), secs = i.scalar(a[2]);
    if (sr <= 0 || secs < 0) i.bad("sr must be > 0 and seconds >= 0");
    double coef = secs <= 0 ? 0.0 : std::exp(-1.0 / (secs * sr));
    varr out(0.0, x.size()); lag_state st;
    if (x.size()) lag_run(&x[0], &out[0], x.size(), coef, st);
    return v_arr(std::move(out));
}

// --- harmonic / percussive separation (HPSS, Fitzgerald 2010) -------------------------------
// (hpss x n hop kernel) => (list harmonic percussive): STFT with a periodic Hann window of n and
// hop; the magnitude spectrogram is median-filtered along time (harmonic sounds are horizontal
// lines) and along frequency (percussive sounds are vertical ones); soft masks from the two
// filtered spectrograms weight the original spectra, which are inverted and overlap-added. kernel
// is the filter length in frames and bins (odd, ~17). The two parts add up to x (signals/hpss.h,
// the engine of Moritz's hpss too)
inline vptr sig_hpss(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); size_t n = (size_t)i.index(a[1]), hop = (size_t)i.index(a[2]); int kernel = (int)i.scalar(a[3]);
    if (n < 16 || n > ((size_t)1 << 24) || (n & (n - 1))) i.bad("n must be a power of 2, 16 to 2^24");
    if (hop < 1 || hop > n) i.bad("hop must be in 1..n");
    if (kernel < 3) i.bad("kernel must be >= 3");
    std::vector<double> h, p;
    dsp::hpss(x.size() ? &x[0] : (const double*)nullptr, x.size(), n, hop, kernel, h, p, [&] { i.yield_check(); return true; });
    return v_list({ v_arr(varr(h.data(), h.size())), v_arr(varr(p.data(), p.size())) });
}

// --- spatial: spherical harmonics, rotation, a spherical-head HRIR --------------------------
// Conventions (AmbiX): azimuth in degrees, 0 in front, positive to the LEFT (counter-clockwise seen
// from above); elevation in degrees, positive up. Channels in ACN order with SN3D normalisation.
using dsp::deg2rad;
using dsp::sh_gains;
using dsp::ambi_decoder;      // signals/ambisonics.h
// (ambi-gains order az el) => the (order+1)^2 encoding gains (SN3D, ACN) of a direction
inline vptr sig_ambi_gains(vlist& a, Interp& i) {
    int order = (int)i.scalar(a[0]); if (order < 0 || order > 7) i.bad("order must be 0..7");
    auto g = sh_gains(order, i.scalar(a[1]), i.scalar(a[2])); varr v(g.size()); for (size_t k = 0; k < g.size(); k++) v[k] = g[k]; return v_arr(std::move(v));
}
// the directions of a list of speakers: each (list az el) or a bare azimuth (a vector of azimuths too)
inline std::vector<std::pair<double, double>> speaker_dirs(const vptr& spk, Interp& i) {
    std::vector<std::pair<double, double>> dirs;
    if (spk->t == Value::NUM) { for (double v : spk->num) dirs.push_back({ v, 0 }); return dirs; }
    if (spk->t != Value::LIST) i.bad("speakers is a list of (list az el) or azimuths");
    for (auto& e : spk->l) {
        if (e->t == Value::LIST && e->l.size() >= 2) dirs.push_back({ i.scalar(e->l[0]), i.scalar(e->l[1]) });
        else dirs.push_back({ i.scalar(e), 0 });
    }
    return dirs;
}
// (ambi-decoder order speakers) => the decoding matrix ambi-decode uses: a row of (order+1)^2 gains per speaker
//   (speakers: (list az el) or azimuths). All at ear height: a ring; evenly spaced with at least 2N+1 speakers, the
//   exact horizontal decoder, else (irregular, as 5.0, or too few speakers) the energy-preserving decoder (EPAD) of
//   the circular harmonics, so every direction is about as loud; both with 2D max-rE weights. With elevations: the
//   energy-preserving decoder of the spherical harmonics with 3D max-rE weights (on a t-design, the sampling decoder)
inline vptr sig_ambi_decoder(vlist& a, Interp& i) {
    int order = (int)i.scalar(a[0]); if (order < 0 || order > 7) i.bad("order must be 0..7");
    auto M = ambi_decoder(order, speaker_dirs(a[1], i));
    vlist out; for (auto& r : M) { varr v(r.size()); for (size_t k = 0; k < r.size(); k++) v[k] = r[k]; out.push_back(v_arr(std::move(v))); }
    return v_list(std::move(out));
}
// (ambi-rotate B yaw) => B turned by yaw degrees about the vertical axis (positive to the left):
//   within each order, the pair (m, -m) rotates by m yaw, exactly; B is a list of (order+1)^2 channel vectors
inline vptr sig_ambi_rotate(vlist& a, Interp& i) {
    vlist& B = i.list(a[0]); double yaw = deg2rad(i.scalar(a[1]));
    size_t n = B.size(); int order = (int)std::lround(std::sqrt((double)n)) - 1;
    if ((size_t)(order + 1) * (order + 1) != n) i.bad("a B-format signal has (order+1)^2 channels");
    vlist out(n);
    for (int l = 0; l <= order; l++) {
        out[(size_t)(l * l + l)] = B[(size_t)(l * l + l)];
        for (int m = 1; m <= l; m++) {
            const varr& cp = i.num(B[(size_t)(l * l + l + m)]); const varr& cm = i.num(B[(size_t)(l * l + l - m)]);
            if (cp.size() != cm.size()) i.bad("channels must have the same length");
            double c = std::cos(m * yaw), s2 = std::sin(m * yaw);
            varr rp(cp.size()), rm(cp.size());
            for (size_t k = 0; k < cp.size(); k++) { rp[k] = c * cp[k] - s2 * cm[k]; rm[k] = s2 * cp[k] + c * cm[k]; }
            out[(size_t)(l * l + l + m)] = v_arr(std::move(rp)); out[(size_t)(l * l + l - m)] = v_arr(std::move(rm));
        }
    }
    return v_list(std::move(out));
}
// (conv x h) (conv-fast x h) => the linear convolution of x and h (length n + m - 1, n and m their lengths), by real FFTs
//   (signals/convolution.h, as Moritz's condec): one transform when x is not much longer than h, else overlap-add
//   over blocks; the interpreter breathes between blocks (the windows stay responsive while a long score goes
//   through the hall). conv streams in a synth (h constant, partitioned); conv-fast is the same function offline
inline vptr sig_conv(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); const varr& h = i.num(a[1]);
    if (x.size() == 0 || h.size() == 0) return v_arr(varr());
    auto y = dsp::convolve(&x[0], x.size(), &h[0], h.size(), [&] { i.yield_check(); i.idle(); });
    return v_arr(varr(y.data(), y.size()));
}
// --- measured HRTFs: a table of directions with the two ears' impulse responses ---
// (hrtf-load path) load a set from a CSV: each row azimuth, elevation, then the left ear's samples and the right's
//   (as src/hrtf_kemar.csv, the MIT KEMAR compact set at 44100 Hz, found on the load path); => the number of directions.
//   Once a set is loaded, hrir takes the nearest measured direction (resampled to the requested rate);
//   (hrtf-unload) goes back to the spherical-head model; (hrtf-loaded) => directions loaded, 0 for the model.
// The set is shared by the interpreters of the process (several run on threads in Moritz): it is held by a shared
// pointer that a load replaces under a lock, so an interpreter keeps the set it started a call with; the responses
// resampled to a rate are kept with the set (under its own lock), each computed once.
struct hrtf_set {
    std::vector<double> az, el; std::vector<varr> left, right; double sr = 44100;
    std::mutex m;                                            // guards what follows
    std::map<long, double> scale;                            // per rate: the energy of the front direction there
    std::map<std::pair<size_t, long>, std::pair<varr, varr>> at_rate;   // (direction, rate) -> the two ears at that rate
};
struct hrtf_registry { std::mutex m, loading; std::shared_ptr<hrtf_set> set; bool unloaded_by_user = false, tried = false; };
inline hrtf_registry& hrtf_reg() { static hrtf_registry r; return r; }
inline std::shared_ptr<hrtf_set> hrtf_current() { auto& r = hrtf_reg(); std::lock_guard<std::mutex> l(r.m); return r.set; }
inline vptr sig_hrtf_load(vlist& a, Interp& i) {
    std::string path = i.find_file(i.str(a[0])); if (path.empty()) path = i.read_path(i.str(a[0]));
    std::ifstream f(path); if (!f) i.bad("cannot open " + i.str(a[0]));
    auto h = std::make_shared<hrtf_set>();
    for (auto& d : dsp::read_hrtf_csv(f)) {                // signals/hrtf.h
        h->az.push_back(d.az); h->el.push_back(d.el);
        h->left.push_back(varr(d.left.data(), d.left.size())); h->right.push_back(varr(d.right.data(), d.right.size()));
    }
    if (h->az.empty()) i.bad("no directions in " + path);
    // normalised so that a source in front has unit energy at each ear (the set's overall level is arbitrary)
    size_t front = 0; double bd = 1e9; for (size_t k = 0; k < h->az.size(); k++) { double d = std::fabs(h->az[k]) + std::fabs(h->el[k]); if (d < bd) { bd = d; front = k; } }
    double e = 0; for (double v : h->left[front]) e += v * v; for (double v : h->right[front]) e += v * v; e = std::sqrt(e / 2); if (e <= 0) e = 1;
    for (auto& v : h->left) v /= e; for (auto& v : h->right) v /= e;
    double n = (double)h->az.size();
    auto& r = hrtf_reg(); std::lock_guard<std::mutex> l(r.m); r.set = std::move(h); r.unloaded_by_user = false;
    return v_num(n);
}
inline vptr sig_hrtf_unload(vlist&, Interp&) { auto& r = hrtf_reg(); std::lock_guard<std::mutex> l(r.m); r.set.reset(); r.unloaded_by_user = true; return v_nil(); }
// the set to use: the loaded one, or the bundled one loaded on the first request if it is on the load path; null for the model
inline std::shared_ptr<hrtf_set> hrtf_try_default(Interp& i) {
    auto& r = hrtf_reg();
    if (auto s = hrtf_current()) return s;
    std::lock_guard<std::mutex> one(r.loading);              // one thread loads, the others wait for it
    { std::lock_guard<std::mutex> l(r.m); if (r.set) return r.set; if (r.tried || r.unloaded_by_user) return nullptr; r.tried = true; }
    std::string p = i.find_file("hrtf_kemar.csv"); if (p.empty()) return nullptr;
    vlist a = { v_str("hrtf_kemar.csv") }; try { sig_hrtf_load(a, i); } catch (...) { return nullptr; }
    return hrtf_current();
}
inline vptr sig_hrtf_loaded(vlist&, Interp& i) { auto s = hrtf_try_default(i); return v_num(s ? (double)s->az.size() : 0.0); }
// (hrir-model az el sr) => the spherical-head model, whatever is loaded (below)
// (hrir az el sr) => (list left right): the nearest measured direction of the loaded set (the MIT KEMAR set is loaded
//   from the load path on first use), resampled to sr; the spherical-head model when no set is available
//   for each ear a head-shadow filter, whose high-frequency roll-off grows with the angle from that ear, and the
//   interaural delay (Woodworth). No measured data: the model externalises and lateralises well enough to work with;
//   real HRTFs can replace it (a WAV pair per direction convolved the same way).
inline vptr sig_hrir_model(vlist& a, Interp& i) {
    double az = deg2rad(i.scalar(a[0])), el = deg2rad(i.scalar(a[1])), sr = i.scalar(a[2]);
    const double head = 0.0875, c = 343.0, w0 = c / head, len_s = 0.003; size_t len = (size_t)(len_s * sr) + 16;
    double x = std::cos(el) * std::cos(az), y = std::cos(el) * std::sin(az);
    vlist ears;
    for (int ear = 0; ear < 2; ear++) {
        double ey = ear == 0 ? 1.0 : -1.0;                 // left ear at +y
        double cos_theta = std::max(-1.0, std::min(1.0, y * ey)); double theta = std::acos(cos_theta);   // angle from the ear axis
        double alpha = 1.05 + 0.95 * std::cos(std::min(theta / deg2rad(150.0), 1.0) * 3.14159265358979323846);   // 2 at the near ear, 0.1 at the far one
        double delay = theta < 3.14159265358979323846 / 2 ? -head / c * std::cos(theta) : head / c * (theta - 3.14159265358979323846 / 2);
        delay += head / c;                                  // shifted so every delay is positive
        // the shadow filter H(s) = (alpha s + 2 w0) / (s + 2 w0), bilinear transform at fs = sr
        double K = 2 * sr, b0 = (alpha * K + 2 * w0), b1 = (2 * w0 - alpha * K), a0 = (K + 2 * w0), a1 = (2 * w0 - K);
        b0 /= a0; b1 /= a0; a1 /= a0;
        varr ir(0.0, len); double d = delay * sr; size_t i0 = (size_t)d; double fr = d - i0;
        std::vector<double> imp(len, 0.0); if (i0 < len) imp[i0] += 1 - fr; if (i0 + 1 < len) imp[i0 + 1] += fr;
        double x1 = 0, y1 = 0; for (size_t k = 0; k < len; k++) { double yk = b0 * imp[k] + b1 * x1 - a1 * y1; x1 = imp[k]; y1 = yk; ir[k] = yk; }
        double e = 0; for (double v : ir) e += v * v; e = std::sqrt(e); if (e > 0) for (auto& v : ir) v /= e;   // unit energy per ear, like a loaded set
        ears.push_back(v_arr(std::move(ir)));
    }
    (void)x; return v_list(std::move(ears));
}
inline vptr sig_hrir(vlist& a, Interp& i) {
    std::shared_ptr<hrtf_set> hp = hrtf_try_default(i);
    if (!hp) return sig_hrir_model(a, i);
    hrtf_set& h = *hp;
    double az = i.scalar(a[0]), el = i.scalar(a[1]), sr = i.scalar(a[2]);
    // nearest direction on the sphere
    double ca = deg2rad(az), ce = deg2rad(el); double x = std::cos(ce) * std::cos(ca), y = std::cos(ce) * std::sin(ca), z = std::sin(ce);
    size_t best = 0; double bd = -2;
    for (size_t k = 0; k < h.az.size(); k++) { double a2 = deg2rad(h.az[k]), e2 = deg2rad(h.el[k]); double d = x * std::cos(e2) * std::cos(a2) + y * std::cos(e2) * std::sin(a2) + z * std::sin(e2); if (d > bd) { bd = d; best = k; } }
    if (std::fabs(sr - h.sr) <= 1) return v_list({ v_arr(h.left[best]), v_arr(h.right[best]) });
    long key = std::lround(sr);
    { std::lock_guard<std::mutex> l(h.m); auto it = h.at_rate.find({ best, key }); if (it != h.at_rate.end()) return v_list({ v_arr(it->second.first), v_arr(it->second.second) }); }
    double f = sr / h.sr; varr L = resample_sinc(h.left[best], f), R = resample_sinc(h.right[best], f);
    // resampling to a lower rate drops the band above its Nyquist and with it energy: renormalise so that
    // the front direction keeps unit energy at this rate (computed once per rate)
    double sc = 0;
    { std::lock_guard<std::mutex> l(h.m); auto it = h.scale.find(key); if (it != h.scale.end()) sc = it->second; }
    if (sc == 0) {
        size_t fr = 0; double fd = 1e9; for (size_t k = 0; k < h.az.size(); k++) { double d = std::fabs(h.az[k]) + std::fabs(h.el[k]); if (d < fd) { fd = d; fr = k; } }
        varr fl = resample_sinc(h.left[fr], f), frr = resample_sinc(h.right[fr], f); double e = 0; for (double v : fl) e += v * v; for (double v : frr) e += v * v;
        sc = std::sqrt(e / 2); if (!(sc > 0)) sc = 1;
        std::lock_guard<std::mutex> l(h.m); h.scale[key] = sc;
    }
    L /= sc; R /= sc;
    { std::lock_guard<std::mutex> l(h.m); h.at_rate[{ best, key }] = { L, R }; }
    return v_list({ v_arr(std::move(L)), v_arr(std::move(R)) });
}
// (binaural-level sr speakers) => the gain that gives a front source, decoded at order 1 to these virtual speakers
//   (ambi-decoder) and rendered with their head responses (hrir), the energy of the front head response itself;
//   binaural-decode uses it (it depends on the rate and on the set of responses loaded)
inline vptr sig_binaural_level(vlist& a, Interp& i) {
    double sr = i.scalar(a[0]); auto dirs = speaker_dirs(a[1], i);
    auto M = ambi_decoder(1, dirs); auto src = sh_gains(1, 0, 0);
    auto resp = [&](double az, double el) { vlist args = { v_num(az), v_num(el), v_num(sr) }; vptr h = sig_hrir(args, i); return std::make_pair(i.num(h->l[0]), i.num(h->l[1])); };
    std::vector<double> L, R;
    for (size_t d = 0; d < dirs.size(); d++) {
        double g = 0; for (size_t k = 0; k < src.size(); k++) g += M[d][k] * src[k];
        auto h = resp(dirs[d].first, dirs[d].second);
        if (L.size() < h.first.size()) L.resize(h.first.size(), 0.0);
        if (R.size() < h.second.size()) R.resize(h.second.size(), 0.0);
        for (size_t k = 0; k < h.first.size(); k++) L[k] += g * h.first[k];
        for (size_t k = 0; k < h.second.size(); k++) R[k] += g * h.second[k];
    }
    auto f = resp(0, 0); double ef = 0, ed = 0;
    for (double v : f.first) ef += v * v; for (double v : f.second) ef += v * v;
    for (double v : L) ed += v * v; for (double v : R) ed += v * v;
    return v_num(std::sqrt(ef / std::max(ed, 1e-12)));
}

// --- music information retrieval: chroma, self-similarity, tempo, beats --------------------------
// the matrix of a builtin: rows of a mir::Mat (rows x frames) as vectors
inline vptr mat_rows(const mir::Mat& m) { vlist out; for (auto& r : m) out.push_back(v_arr(varr(r.data(), r.size()))); return v_list(std::move(out)); }
inline mir::Mat rows_of(Interp& i, const vptr& v, bool square) {
    if (v->t != Value::LIST || v->l.empty()) i.bad("a matrix (a list of row vectors)");
    mir::Mat m; size_t cols = i.num(v->l[0]).size();
    for (auto& r : v->l) { const varr& x = i.num(r); if (x.size() != cols) i.bad("rows of different lengths"); m.emplace_back(std::begin(x), std::end(x)); }
    if (square && cols != m.size()) i.bad("not square");
    return m;
}
inline void check_stft(Interp& i, size_t n, size_t hop) {
    if (n < 16 || n > ((size_t)1 << 24) || (n & (n - 1))) i.bad("n must be a power of 2, 16 to 2^24");
    if (hop < 1 || hop > n) i.bad("hop must be in 1..n");
}
// the magnitude spectrogram of the analyses below: mir.h's (Hann n, a frame every hop centred on it, a full-scale
// sine 1), frames x (n/2 + 1)
inline mir::Mat mag_spectrogram(Interp& i, const varr& x, size_t n, size_t hop) {
    std::vector<double> xs(std::begin(x), std::end(x));
    return mir::magSpectrogram(xs, (int)n, (int)hop, [&](double) { i.yield_check(); return true; });
}
// (chromagram x sr n hop) => a 12 x frames matrix (rows C C# D ... B), a frame every hop samples, the first at 0.
//   Read from the spectral peaks (as HPCP) of the magnitude spectrogram (Hann n): the local maxima above -80 dB and
//   above 1/1000 of the frame's strongest, their frequency refined by a parabola through the peak and its
//   neighbours (on the log magnitudes), each given to the pitch class of its nearest note between C2 and B6, so a
//   tone counts once; a peak that is probably the 3rd, 5th, 6th or 7th partial of a lower one (at that fraction of
//   its frequency within a third of a semitone, and at least 0.3 as loud) counts a tenth, so overtones do not pull
//   the harmony a fifth or a third up. Every frame scaled to a maximum of 1 (a silent frame stays 0)
inline vptr sig_chromagram(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); double sr = i.scalar(a[1]); size_t n = (size_t)i.index(a[2]), hop = (size_t)i.index(a[3]);
    check_stft(i, n, hop);
    auto mag = mag_spectrogram(i, x, n, hop); auto map = mir::chromaMap(sr, (int)n);
    mir::Mat rows(12, mir::Vec(mag.size(), 0.0)); mir::Vec c;
    for (size_t f = 0; f < mag.size(); f++) { mir::chroma(mag[f], map, c); for (int k = 0; k < 12; k++) rows[(size_t)k][f] = c[(size_t)k]; }
    return mat_rows(rows);
}
// (self-similarity F) => the frames x frames matrix of the cosine similarity between the columns of a feature
//   matrix F (rows are features, columns frames: a chromagram, a spectrogram); 1 on the diagonal, 0 for frames
//   with nothing in common (Foote 1999: the structure of a piece as a picture, repeats as stripes off the diagonal)
inline vptr sig_self_similarity(vlist& a, Interp& i) {
    return mat_rows(mir::selfSimilarity(rows_of(i, a[0], false), [&](double) { i.yield_check(); return true; }));
}
// (foote-novelty S width) => a vector, one value per frame: the self-similarity matrix correlated along its
//   diagonal with a checkerboard kernel of width x width (Gaussian-tapered; width even, ~16-64 frames): the mean
//   similarity within the past and within the future minus the mean similarity between them, each weighted by the
//   kernel and taken only over what lies inside the matrix (0 near the ends, where less than half of the kernel
//   fits). High where what comes before is unlike what comes after, the boundaries of the sections (Foote 2000)
inline vptr sig_foote_novelty(vlist& a, Interp& i) {
    int width = (int)i.scalar(a[1]); if (width < 2) i.bad("width must be >= 2");
    auto nov = mir::footeNovelty(rows_of(i, a[0], true), width);
    return v_arr(varr(nov.data(), nov.size()));
}
// (tempogram flux fps window [step]) => a matrix bpm x columns: every step frames (1 by default: a column per frame;
//   (/ fps 4) gives four columns a second, much faster on a long piece), the autocorrelation of the onset strength
//   (a vector, fps values a second) over a Hann window of window frames around the frame, read at the lags of the
//   tempi 30..300 bpm (one row per bpm; a lag between two frames interpolated; only the lags of that range are
//   computed). The strength is first taken above its local mean (0.3 s) and rectified: its peaks, without the slow
//   decay of a big hit, which would look periodic at every tempo. Every column is divided by the autocorrelation at
//   lag 0 (the window's energy), so 1 is a perfectly periodic pulse and a passage without one stays dark
inline vptr sig_tempogram(vlist& a, Interp& i) {
    const varr& flux = i.num(a[0]); double fps = i.scalar(a[1]); int win = (int)i.index(a[2]);
    int step = a.size() > 3 ? (int)std::lround(i.scalar(a[3])) : 1;
    if (win < 8) i.bad("window must be >= 8");
    if (fps <= 0) i.bad("fps must be > 0");
    return mat_rows(mir::tempogram(mir::Vec(std::begin(flux), std::end(flux)), fps, win, step));
}
// (beat-track flux fps bpm tightness) => the frames of the beats (a vector): dynamic programming over the onset
//   strength (fps values a second) for the sequence of beats that is strong and keeps near the period of bpm
//   (tightness, ~100: how much a deviation from the period costs, on a log scale; Ellis 2007)
inline vptr sig_beat_track(vlist& a, Interp& i) {
    const varr& o = i.num(a[0]); double fps = i.scalar(a[1]), bpm = i.scalar(a[2]), tight = i.scalar(a[3]);
    auto beats = mir::beatTrack(mir::Vec(std::begin(o), std::end(o)), fps, bpm, tight);
    varr out(0.0, beats.size()); for (size_t k = 0; k < beats.size(); k++) out[k] = beats[k];
    return v_arr(std::move(out));
}


// (mel-spectrogram x sr n hop nmel) => an nmel x frames matrix: the energy in dB of the magnitude spectrogram (Hann
//   n, hop, a frame every hop samples, scaled so that a full-scale sine is 0 dB) through nmel triangular filters
//   evenly spaced on the mel scale from 0 to sr/2 (a band narrower than a bin takes its nearest bin), floored at
//   -100 dB for silence: a spectrogram at the ear's resolution, the timbre of every frame
inline mir::Mat log_mel(Interp& i, const varr& x, double sr, size_t n, size_t hop, int nmel) {
    auto mag = mag_spectrogram(i, x, n, hop); mir::MelBank bank; bank.init(sr, (int)n, nmel);
    mir::Mat rows((size_t)nmel, mir::Vec(mag.size(), 0.0)); mir::Vec mel;
    for (size_t f = 0; f < mag.size(); f++) { bank.apply(mag[f], mel); for (int m = 0; m < nmel; m++) rows[(size_t)m][f] = mel[(size_t)m]; }
    return rows;
}
inline vptr sig_mel_spectrogram(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); double sr = i.scalar(a[1]); size_t n = (size_t)i.index(a[2]), hop = (size_t)i.index(a[3]); int nmel = (int)i.index(a[4]);
    check_stft(i, n, hop); if (nmel < 2) i.bad("nmel must be >= 2");
    return mat_rows(log_mel(i, x, sr, n, hop, nmel));
}
// (mfcc-gram x sr n hop nmel ncoeff) => an ncoeff x frames matrix: the mel-frequency cepstral coefficients of every
//   frame (the DCT-II of the mel spectrogram in dB, nmel filters, the first ncoeff coefficients: a smooth description of
//   the spectral envelope, the timbre; coefficient 0 is the overall level)
inline vptr sig_mfcc_gram(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); double sr = i.scalar(a[1]); size_t n = (size_t)i.index(a[2]), hop = (size_t)i.index(a[3]); int nmel = (int)i.index(a[4]), nc = (int)i.index(a[5]);
    check_stft(i, n, hop);
    if (nmel < 2 || nc < 1 || nc > nmel) i.bad("need 2 <= ncoeff <= nmel");
    auto mel = log_mel(i, x, sr, n, hop, nmel); size_t frames = mel[0].size();
    mir::Mat rows((size_t)nc, mir::Vec(frames, 0.0)); mir::Vec col((size_t)nmel), c;
    for (size_t f = 0; f < frames; f++) { for (int m = 0; m < nmel; m++) col[(size_t)m] = mel[(size_t)m][f]; mir::dct(col, nc, c); for (int k = 0; k < nc; k++) rows[(size_t)k][f] = c[(size_t)k]; }
    return mat_rows(rows);
}
// (key-scores chroma)      every key against a pitch-class distribution (a vector of 12, C first: chroma-mean of a
//   chromagram): the correlation of the distribution with the Krumhansl-Kessler profile of each major and minor key
//   (Krumhansl 1990; mir.h's keyScores, as Moritz's keyfind); => (list (list "D" 'minor r) ...), best first. Keys
//   are spelled as musicians spell them: Db Eb F# Ab Bb major, C# Eb F# G# Bb minor
inline vptr sig_key_scores(vlist& a, Interp& i) {
    const varr& c = i.num(a[0]); if (c.size() != 12) i.bad("a pitch-class distribution of 12 values");
    auto r = mir::keyScores(mir::Vec(std::begin(c), std::end(c)));
    std::vector<size_t> idx(r.size()); for (size_t k = 0; k < idx.size(); k++) idx[k] = k;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t p, size_t q) { return r[p] < r[q]; });
    std::reverse(idx.begin(), idx.end());                    // best first; ties in reverse order, as before
    vlist out;
    for (size_t k : idx) out.push_back(v_list({ v_str(mir::keyName((int)(k / 2), k % 2)), v_sym(k % 2 ? "minor" : "major"), v_num(r[k]) }));
    return v_list(std::move(out));
}
// (tempo-estimate-with flux fps centre width)   the tempo of an onset strength (a vector, fps values a second): its
//   autocorrelation (the mean removed) read at the lags of 30..300 bpm (half-bpm steps, a lag under half the curve's
//   length), weighted by a log-normal prior on the tempo a listener taps, centred on centre bpm, width octaves wide
//   (mir.h's tempoEstimate, as Moritz's beats); (tempo-estimate-with flux fps 120 10) is the autocorrelation's own peak
inline vptr sig_tempo_estimate_with(vlist& a, Interp& i) {
    const varr& f = i.num(a[0]);
    double fps = i.scalar(a[1]);
    if (!std::isfinite(fps) || fps <= 0 || fps > 1e6) i.bad("fps must be in (0, 1e6]");
    return v_num(mir::tempoEstimate(mir::Vec(std::begin(f), std::end(f)), i.scalar(a[1]), i.scalar(a[2]), i.scalar(a[3])));
}

// --- pitch and low-level descriptors (signals/pitch.h, signals/mir.h: Moritz's descriptors~) ----------------
inline const double* data_of(const varr& v) { return v.size() ? &v[0] : nullptr; }
// a frame of n samples centred on sample c of x (zeros outside), as the spectrograms above read them
inline void centred_frame(const varr& x, long c, size_t n, std::vector<double>& buf) {
    long start = c - (long)n / 2;
    for (size_t k = 0; k < n; k++) { long j = start + (long)k; buf[k] = (j >= 0 && j < (long)x.size()) ? x[(size_t)j] : 0.0; }
}
// YIN's options from a[from]: threshold (0.1), fmin (50 Hz), fmax (4000 Hz)
inline void yin_options(vlist& a, size_t from, Interp& i, double sr, double& thr, double& fmin, double& fmax) {
    thr = a.size() > from ? i.scalar(a[from]) : 0.1;
    fmin = a.size() > from + 1 ? i.scalar(a[from + 1]) : 50.0;
    fmax = a.size() > from + 2 ? i.scalar(a[from + 2]) : 4000.0;
    if (!std::isfinite(sr) || sr <= 0 || sr > 1e7) i.bad("sr must be in (0, 1e7]");
    if (!(thr > 0 && thr <= 1)) i.bad("threshold must be in (0, 1]");
    if (!(fmin > 0) || !(fmax > fmin) || !std::isfinite(fmax)) i.bad("need 0 < fmin < fmax");
}
inline void yin_ready(Interp& i, const dsp::Yin& y) {
    if (!y.ready()) i.bad("the frame is too short for fmax (it needs at least 4 sr / fmax + 8 samples)");
}
// (yin x sr [threshold] [fmin] [fmax]) => (list f0 periodicity): the fundamental of x read as one frame by YIN
//   (de Cheveigné & Kawahara 2002; signals/pitch.h, the f0 of Moritz's descriptors~): the difference function of
//   x against itself, by FFT, over windows of half its length, normalised by its cumulative mean; the first lag
//   whose value dips under threshold (0.1; lower is stricter) is the period, refined by a parabola and checked
//   against the windows around the centre (the best local estimate). f0 is 0 when no lag dips under the
//   threshold (noise, silence); periodicity is 1 minus the normalised difference at the period (1 for a perfectly
//   periodic sound, about 0 for noise and silence). f0 is searched between fmin (50 Hz) and fmax (4000 Hz), and no
//   lower than 2 sr / (length x): a 2048-sample frame at 44.1 kHz reaches 43 Hz. Unlike acf-f0, a weak or missing
//   fundamental does not make it jump an octave up
inline vptr sig_yin(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); double sr = i.scalar(a[1]), thr, fmin, fmax;
    yin_options(a, 2, i, sr, thr, fmin, fmax);
    if (x.size() > ((size_t)1 << 20)) i.bad("x is longer than 2^20 samples (a frame: yin-track follows a sound)");
    dsp::Yin y; y.init((int)x.size(), sr, fmin, fmax); yin_ready(i, y);
    auto p = y.estimate(&x[0], thr);
    return v_list({ v_num(p.f0), v_num(p.periodicity) });
}
// (yin-track x sr n hop [threshold] [fmin] [fmax]) => (list f0s periodicities): yin of every frame of n samples (a
//   power of 2), a frame every hop samples centred on it (the first at 0, as the spectrograms): two vectors, one
//   value per frame; f0 0 where the frame is unvoiced
inline vptr sig_yin_track(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); double sr = i.scalar(a[1]), thr, fmin, fmax;
    size_t n = (size_t)i.index(a[2]), hop = (size_t)i.index(a[3]);
    check_stft(i, n, hop); yin_options(a, 4, i, sr, thr, fmin, fmax);
    dsp::Yin y; y.init((int)n, sr, fmin, fmax); yin_ready(i, y);
    size_t frames = (size_t)mir::frameCount((long)x.size(), (int)hop);
    varr f0(0.0, frames), per(0.0, frames); std::vector<double> buf(n);
    for (size_t f = 0; f < frames; f++) {
        centred_frame(x, (long)(f * hop), n, buf);
        auto p = y.estimate(buf.data(), thr);
        f0[f] = p.f0; per[f] = p.periodicity;
        if (f % 16 == 0) i.yield_check();
    }
    return v_list({ v_arr(std::move(f0)), v_arr(std::move(per)) });
}
// (descriptors x sr n hop [threshold] [fmin] [fmax]) => an 11 x frames matrix, the low-level descriptors of every
//   frame of n samples (a power of 2), a frame every hop samples centred on it (the first at 0), in the order and
//   with the formulas of Moritz's descriptors~ (mir.h): rms; the spectral centroid and spread (Hz), skewness and
//   kurtosis of the magnitudes (Hann window, a full-scale sine 1); the flux (the rectified increase of the
//   magnitudes since the frame before; the first from silence); hfc (the energy weighted by bin number over the sum
//   of the bin numbers); zcr (zero crossings a second); flatness (geometric over arithmetic mean of the
//   magnitudes, 0 for silence); then yin's periodicity and f0 (0 when unvoiced)
inline vptr sig_descriptors(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]); double sr = i.scalar(a[1]), thr, fmin, fmax;
    size_t n = (size_t)i.index(a[2]), hop = (size_t)i.index(a[3]);
    check_stft(i, n, hop); yin_options(a, 4, i, sr, thr, fmin, fmax);
    mir::FrameDescriptors fd; fd.init((int)n, sr, thr, fmin, fmax);
    mir::FrameSpectrum fs; fs.init((int)n);
    bool pitch = fd.yin().ready();
    size_t frames = (size_t)mir::frameCount((long)x.size(), (int)hop);
    mir::Mat rows(mir::kDescriptorCount, mir::Vec(frames, 0.0));
    std::vector<double> buf(n); mir::Vec mag; double v[mir::kDescriptorCount];
    for (size_t f = 0; f < frames; f++) {
        centred_frame(x, (long)(f * hop), n, buf);
        fs.magnitudes(buf.data(), mag);
        fd.compute(buf.data(), mag, v, pitch);
        for (int k = 0; k < mir::kDescriptorCount; k++) rows[(size_t)k][f] = v[k];
        if (f % 16 == 0) i.yield_check();
    }
    return mat_rows(rows);
}
// (spectral-moments amps freqs) => (vec centroid spread skewness kurtosis) of magnitudes amps at the frequencies
//   freqs (as long): the amplitude-weighted mean frequency, the standard deviation around it, and the third and
//   fourth standardised moments (kurtosis 3 for a Gaussian shape); all 0 for silence, the last two 0 for a single
//   line (mir.h, as descriptors~); spectral-centroid, spectral-spread, spectral-skewness and spectral-kurtosis read it
inline vptr sig_spectral_moments(vlist& a, Interp& i) {
    const varr& m = i.num(a[0]); const varr& f = i.num(a[1]);
    if (m.size() != f.size()) i.bad("amps and freqs must have the same length");
    auto r = mir::spectralMoments(data_of(m), m.size(), [&f](size_t k) { return f[k]; });
    varr out(4); out[0] = r.centroid; out[1] = r.spread; out[2] = r.skewness; out[3] = r.kurtosis;
    return v_arr(std::move(out));
}
// (spectral-flatness amps)   geometric over arithmetic mean of the magnitudes: 1 for white noise, near 0 for a pure
//   tone, 0 for silence (mir.h, as descriptors~)
inline vptr sig_spectral_flatness(vlist& a, Interp& i) {
    const varr& m = i.num(a[0]);
    return v_num(mir::spectralFlatness(data_of(m), m.size()));
}
// (spectral-flux amps previous)   the rectified increase of the magnitudes from the previous frame (previous as long,
//   or a number) (mir.h, as descriptors~ and onset~)
inline vptr sig_spectral_flux(vlist& a, Interp& i) {
    const varr& m = i.num(a[0]); const varr& p = i.num(a[1]);
    if (p.size() != m.size() && p.size() != 1) i.bad("previous must be as long as amps (or a number)");
    mir::Vec cur(std::begin(m), std::end(m)), prev(m.size(), p.size() == 1 ? p[0] : 0.0);
    if (p.size() == m.size()) prev.assign(std::begin(p), std::end(p));
    return v_num(mir::spectralFlux(cur, prev));
}
// --- morphology: a sound's descriptors as curves over time (signals/morphology.h, what a morphological orchestration
// follows and the adaptive segmentation cuts at) ---
inline std::vector<double> doubles_from(Interp& i, const vptr& v) { const varr& x = i.num(v); return std::vector<double>(std::begin(x), std::end(x)); }
inline vptr vec_from(const std::vector<double>& v) { varr a(v.size()); for (size_t k = 0; k < v.size(); k++) a[k] = v[k]; return v_arr(std::move(a)); }
inline size_t positive_size(Interp& i, const vptr& v, const char* what) {
    double d = i.scalar(v);
    if (!(d >= 1) || d > (double)(1 << 24)) i.bad(std::string(what) + " must be a whole number from 1");
    return (size_t)d;
}
// (frame-spectra x block hop ncoeff)   the magnitude spectrum of every frame (a Hann window of block samples every hop,
//   the first frame centred on the start), the first ncoeff bins: the same space as a database's spectrum features,
//   one spectrum per frame instead of the average
inline vptr sig_frame_spectra(vlist& a, Interp& i) {
    size_t block = positive_size(i, a[1], "block"), hop = positive_size(i, a[2], "hop"), n = positive_size(i, a[3], "ncoeff");
    morph::spectra f;
    try { f = morph::frame_spectra(doubles_from(i, a[0]), block, hop, n); } catch (std::runtime_error& e) { i.bad(e.what()); }
    vlist out; for (auto& m : f) out.push_back(vec_from(m));
    return v_list(std::move(out));
}
// (onset-strength x n hop)   the spectral flux of every frame, as a vector (one value per hop)
inline vptr sig_onset_strength(vlist& a, Interp& i) {
    return vec_from(morph::onset_strength(doubles_from(i, a[0]), positive_size(i, a[1], "n"), positive_size(i, a[2], "hop")));
}
// (flux-peaks x sr n hop threshold width)   the times (seconds) of the peaks of the spectral flux (the rectified change
//   of the magnitude spectrum, n and hop as onset-strength) that stand above threshold times the flux's moving median
//   over width frames (odd): every noticeable spectral change, an attack or not, counted as the target's activity;
//   threshold 2 keeps the attacks, 1.5 admits the ripples of a held sound too; peaks within 50 ms of the previous one
//   count as one; => a vector
inline vptr sig_flux_peaks(vlist& a, Interp& i) {
    double sr = i.scalar(a[1]); if (!(sr > 0)) i.bad("the sample rate must be > 0");
    std::vector<double> out;
    try { out = morph::flux_peaks(doubles_from(i, a[0]), sr, positive_size(i, a[2], "n"), positive_size(i, a[3], "hop"), i.scalar(a[4]), i.index(a[5])); }
    catch (std::runtime_error& e) { i.bad(e.what()); }
    return vec_from(out);
}
// (adaptive-onsets x sr n hop ratio timegate)   the onsets of the adaptive segmentation (mimetic-target 'segmentation
//   'adaptive, Moritz's orchidea segmentation adaptive): the flux peaks above ratio times their moving median (as
//   flux-peaks, over 21 frames), at least timegate seconds apart; (vec 0) when there is none
inline vptr sig_adaptive_onsets(vlist& a, Interp& i) {
    double sr = i.scalar(a[1]); if (!(sr > 0)) i.bad("the sample rate must be > 0");
    std::vector<double> out;
    try { out = morph::adaptive_onsets(doubles_from(i, a[0]), sr, positive_size(i, a[2], "n"), positive_size(i, a[3], "hop"), i.scalar(a[4]), i.scalar(a[5])); }
    catch (std::runtime_error& e) { i.bad(e.what()); }
    return vec_from(out);
}
// (peak-density times secs window hop)   how many of some times (a vector, seconds) fall a second around every instant:
//   the count within window seconds centred on each hop-second step over secs seconds, divided by the window; => a
//   vector, one value per step (a density in events a second)
inline vptr sig_peak_density(vlist& a, Interp& i) {
    double secs = i.scalar(a[1]), hop = i.scalar(a[3]);
    if (!(hop > 0) || !(secs / hop < 1e8)) i.bad("hop must be > 0 (and secs / hop under 1e8)");
    return vec_from(morph::peak_density(doubles_from(i, a[0]), secs, i.scalar(a[2]), hop));
}
// (register-curve spectra sr block)   where the sound sits: for each frame the centroid in MIDI pitch (the powers
//   weighting the pitch of every bin above 30 Hz, so an octave is an octave), the spread around it in octaves, and the
//   lowest strong partial in MIDI pitch (a tone's fundamental, more or less: where the register begins). A frame below
//   -60 dB (its peak under 1/1000 of a full-scale sine's, block/4) keeps the values of the frame before (60, 1 and 48
//   at the start), so quiet noise does not make the curves jump; smooth them with median-filter;
//   => (list centroids spreads lows)
inline vptr sig_register_curve(vlist& a, Interp& i) {
    if (a[0]->t != Value::LIST || a[0]->l.empty()) i.bad("spectra: a list of vectors");
    morph::spectra f; for (auto& m : a[0]->l) f.push_back(doubles_from(i, m));
    morph::register_curves r;
    try { r = morph::register_curve(f, i.scalar(a[1]), i.scalar(a[2])); } catch (std::runtime_error& e) { i.bad(e.what()); }
    return v_list({ vec_from(r.centroid), vec_from(r.spread), vec_from(r.low) });
}
// (loudness-curve x sr hop)   the level of every hop in dB, roughly as the ear weighs it (the extreme bass and treble
//   taken out); => a vector, one value per hop, -100 for silence
inline vptr sig_loudness_curve(vlist& a, Interp& i) {
    double sr = i.scalar(a[1]); if (!(sr > 0)) i.bad("the sample rate must be > 0");
    return vec_from(morph::loudness_curve(doubles_from(i, a[0]), sr, positive_size(i, a[2], "hop")));
}
// (hfc amps)   high frequency content: the sum of amps^2 times the bin number over the sum of the bin numbers (mir.h,
//   as descriptors~)
inline vptr sig_hfc(vlist& a, Interp& i) {
    const varr& m = i.num(a[0]);
    return v_num(mir::highFrequencyContent(data_of(m), m.size()));
}
// (zcr x)   the zero-crossing rate, per sample: how many times x changes sign (0 counts as positive) over its length
//   (mir.h; descriptors~ gives it per second)
inline vptr sig_zcr(vlist& a, Interp& i) {
    const varr& x = i.num(a[0]);
    return v_num(x.size() ? (double)mir::zeroCrossings(data_of(x), x.size()) / (double)x.size() : 0.0);
}

// (fade-in x n) (fade-out x n)   linear fades over the first (last) n samples (signals/edit.h: sample k of a fade in
//   times k / n, a fade out the same backwards; Moritz's sound editor fades with it)
inline vptr sig_fade(vlist& a, Interp& i, bool in) {
    varr x = i.num(a[0]);
    double n = i.scalar(a[1]);
    if (!(n >= 0)) i.bad("n must be >= 0");
    if (x.size()) {
        if (in) dsp::edit::fade_in(&x[0], x.size(), 1, (size_t)std::min(n, (double)x.size()));
        else dsp::edit::fade_out(&x[0], x.size(), 1, (size_t)std::min(n, (double)x.size()));
    }
    return v_arr(std::move(x));
}
inline vptr sig_fade_in(vlist& a, Interp& i) { return sig_fade(a, i, true); }
inline vptr sig_fade_out(vlist& a, Interp& i) { return sig_fade(a, i, false); }
// (normalize-peak x)   x scaled so its peak is 1; silence stays silent (signals/edit.h, as the sound editor's Normalize)
inline vptr sig_normalize_peak(vlist& a, Interp& i) {
    varr x = i.num(a[0]);
    if (x.size()) dsp::edit::normalize_peak(&x[0], x.size(), 1);
    return v_arr(std::move(x));
}

inline void add_signals(Interp& i) {
    i.def("fft", sig_fft, 1, 1); i.def("ifft", sig_ifft, 1, 1);
    i.def("osc", sig_osc, 3, 3); i.def("iir", sig_iir, 3, 3); i.def("delay", sig_delay, 2, 2);
    i.def("resample", sig_resample, 2, 2); i.def("autocorr", sig_autocorr, 1, 1);
    i.def("interleave", sig_interleave, 1, 1); i.def("deinterleave", sig_deinterleave, 2, 2);
    i.def("local-maxima", sig_local_maxima, 1, 1); i.def("gather", sig_gather, 2, 2);
    i.def("add-at!", sig_add_at_inplace, 3, 3); i.def("comb", sig_comb, 3, 3); i.def("allpass", sig_allpass, 3, 3);
    i.def("pvoc", sig_pvoc, 2, 2); i.def("nebula", sig_nebula, 3, 3); i.def("harmonize", sig_harmonize, 3, 4); i.def("harmonize-voices", sig_harmonize_voices, 3, 4); i.def("harmonize-f0", sig_harmonize_f0, 2, 3); i.def("scale-steps", sig_scale_steps, 1, 1); i.def("scale-names", sig_scale_names, 0, 0); i.def("deconv", sig_deconv, 2, 3); i.def("svf", sig_svf, 5, 5); i.def("biquad", sig_biquad, 5, 5); i.def("spectral-envelope", sig_spectral_envelope, 2, 2);
    i.def("adsr", sig_adsr, 6, 6); i.def("lag", sig_lag, 3, 3); i.def("hpss", sig_hpss, 4, 4);
    i.def("chromagram", sig_chromagram, 4, 4); i.def("self-similarity", sig_self_similarity, 1, 1); i.def("foote-novelty", sig_foote_novelty, 2, 2);
    i.def("tempogram", sig_tempogram, 3, 4); i.def("beat-track", sig_beat_track, 4, 4);
    i.def("mel-spectrogram", sig_mel_spectrogram, 5, 5); i.def("key-scores", sig_key_scores, 1, 1); i.def("tempo-estimate-with", sig_tempo_estimate_with, 4, 4); i.def("mfcc-gram", sig_mfcc_gram, 6, 6);
    i.def("yin", sig_yin, 2, 5); i.def("yin-track", sig_yin_track, 4, 7); i.def("descriptors", sig_descriptors, 4, 7);
    i.def("spectral-moments", sig_spectral_moments, 2, 2); i.def("spectral-flatness", sig_spectral_flatness, 1, 1);
    i.def("spectral-flux", sig_spectral_flux, 2, 2); i.def("frame-spectra", sig_frame_spectra, 4, 4); i.def("onset-strength", sig_onset_strength, 3, 3);
    i.def("flux-peaks", sig_flux_peaks, 6, 6); i.def("adaptive-onsets", sig_adaptive_onsets, 6, 6); i.def("peak-density", sig_peak_density, 4, 4); i.def("register-curve", sig_register_curve, 3, 3); i.def("loudness-curve", sig_loudness_curve, 3, 3); i.def("hfc", sig_hfc, 1, 1); i.def("zcr", sig_zcr, 1, 1);
    i.def("conv", sig_conv, 2, 2); i.def("conv-fast", sig_conv, 2, 2); i.def("ambi-gains", sig_ambi_gains, 3, 3); i.def("ambi-decoder", sig_ambi_decoder, 2, 2); i.def("binaural-level", sig_binaural_level, 2, 2); i.def("ambi-rotate", sig_ambi_rotate, 2, 2); i.def("hrir", sig_hrir, 3, 3); i.def("hrir-model", sig_hrir_model, 3, 3);
    i.def("fade-in", sig_fade_in, 2, 2); i.def("fade-out", sig_fade_out, 2, 2); i.def("normalize-peak", sig_normalize_peak, 1, 1);
    i.def("hrtf-load", sig_hrtf_load, 1, 1); i.def("hrtf-unload", sig_hrtf_unload, 0, 0); i.def("hrtf-loaded", sig_hrtf_loaded, 0, 0);
}

} // namespace musil
