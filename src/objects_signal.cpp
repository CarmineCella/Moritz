// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_signal.cpp — oscillators, filters, signal math, delays, audio I/O

#include "objects.h"
#include "zoom.h"
#include "signals/filters.h"   // Musil's svf

#include <FL/fl_draw.H>

#include <atomic>
#include <cstring>

namespace moritz {
namespace {

// ---------------------------------------------------------------- cycle~
// cycle~ [freq]: inlet 0 frequency (signal or float), inlet 1 phase (0..1)
class CycleBox : public Box {
public:
    explicit CycleBox(const Message& a) {
        setPorts({P::Signal, P::Control}, {P::Signal});
        setScalar(0, argf(a, 0, 0.f));
        osc_.setPhase(argf(a, 1, 0.f));
    }
    void message(int in, const Message& m) override {
        if (!m[0].isFloat()) return;
        if (in == 0) setScalar(0, m[0].f);
        else osc_.setPhase(m[0].f);
    }
    void dsp(int n) override { osc_.process(sigIn(0, n), sigOut(0), n, patch->root()->sampleRate); }

private:
    dsp::Cycle osc_;
};

// ---------------------------------------------------------------- reson~
// reson~ [gain cf Q]: inlets signal in, gain, centre frequency, Q.
// A list "gain cf Q" in the left inlet sets all three.
class ResonBox : public Box {
public:
    explicit ResonBox(const Message& a) {
        setPorts({P::Signal, P::Signal, P::Signal, P::Signal}, {P::Signal});
        setScalar(1, argf(a, 0, 1.f));
        setScalar(2, argf(a, 1, 1000.f));
        setScalar(3, argf(a, 2, 10.f));
    }
    void message(int in, const Message& m) override {
        if (m[0].is("clear")) { std::lock_guard<std::mutex> lk(patch->mutex()); f_.clear(); return; }
        if (in == 0 && m.size() >= 2) {
            for (size_t k = 0; k < m.size() && k < 3; ++k)
                if (m[k].isFloat()) setScalar(1 + (int)k, m[k].f);
            return;
        }
        if (m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float g = sigIn(1, n)[0], cf = sigIn(2, n)[0], q = sigIn(3, n)[0];
        f_.process(x, sigOut(0), n, g, cf, q, patch->root()->sampleRate);
    }

private:
    dsp::Reson f_;
};

// ---------------------------------------------------------------- noise~
class NoiseBox : public Box {
public:
    NoiseBox() : noise_(seed()) { setPorts({P::Control}, {P::Signal}); }
    void dsp(int n) override { noise_.process(sigOut(0), n); }

private:
    static uint32_t seed() {
        static uint32_t s = 0x12345678u;
        s = s * 1664525u + 1013904223u;
        return s;
    }
    dsp::Noise noise_;
};

// ---------------------------------------------------------------- click~
// click~: bang outputs an impulse; a list of floats ("set" optional) sets
// the impulse shape.
class ClickBox : public Box {
public:
    explicit ClickBox(const Message& a) {
        setPorts({P::Control}, {P::Signal});
        if (!a.empty()) setShape(a);
    }
    void message(int, const Message& m) override {
        if (m[0].is("bang")) { click_.trigger(); return; }
        setShape(m[0].is("set") ? Message(m.begin() + 1, m.end()) : m);
    }
    void dsp(int n) override { click_.process(sigOut(0), n); }

private:
    void setShape(const Message& m) {
        std::vector<float> s;
        for (auto& a : m) if (a.isFloat()) s.push_back(a.f);
        std::lock_guard<std::mutex> lk(patch ? patch->mutex() : dummy_);
        click_.setShape(std::move(s));
    }
    std::mutex dummy_;
    dsp::Click click_;
};

// ---------------------------------------------------------------- +~ -~ *~ /~
class SigOpBox : public Box {
public:
    SigOpBox(dsp::Op op, const Message& a) : op_(op) {
        setPorts({P::Signal, P::Signal}, {P::Signal});
        setScalar(1, argf(a, 0, 0.f));
    }
    void message(int in, const Message& m) override {
        if (m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override { dsp::binop(op_, sigIn(0, n), sigIn(1, n), sigOut(0), n); }

private:
    dsp::Op op_;
};


// ---------------------------------------------------------------- adc~ / dac~
std::vector<int> channelArgs(const Message& a) {
    std::vector<int> ch;
    for (auto& x : a)
        if (x.isFloat() && x.f >= 1.f) ch.push_back((int)x.f);
    if (ch.empty()) ch = {1, 2};
    return ch;
}

class AdcBox : public Box {
public:
    explicit AdcBox(const Message& a) : ch_(channelArgs(a)) {
        setPorts({P::Control}, std::vector<Port>(ch_.size(), P::Signal));
    }
    bool wantsInput() const override { return true; }
    void message(int, const Message& m) override { handleDspMessage(patch, m); }
    void dsp(int n) override {
        for (size_t k = 0; k < ch_.size(); ++k) {
            float* out = sigOut((int)k);
            int c = ch_[k] - 1;
            if (patch->root()->ioIn && c < patch->root()->ioInCh) {
                const float* in = patch->root()->ioIn + (size_t)patch->root()->ioOffset * patch->root()->ioInCh + c;
                for (int i = 0; i < n; ++i) out[i] = in[(size_t)i * patch->root()->ioInCh];
            } else {
                dsp::fill(out, 0.f, n);
            }
        }
    }

private:
    std::vector<int> ch_;
};

class DacBox : public Box {
public:
    explicit DacBox(const Message& a) : ch_(channelArgs(a)) {
        setPorts(std::vector<Port>(ch_.size(), P::Signal), {});
    }
    void message(int, const Message& m) override { handleDspMessage(patch, m); }
    int outputChannels() const override {
        int m = 0;
        for (int c : ch_) m = std::max(m, c);
        return m;
    }
    void dsp(int n) override {
        if (!patch->root()->ioOut) return;
        for (size_t k = 0; k < ch_.size(); ++k) {
            int c = ch_[k] - 1;
            if (c >= patch->root()->ioOutCh) continue;
            const float* in = sigIn((int)k, n);
            float* out = patch->root()->ioOut + (size_t)patch->root()->ioOffset * patch->root()->ioOutCh + c;
            for (int i = 0; i < n; ++i) out[(size_t)i * patch->root()->ioOutCh] += in[i];
        }
    }

private:
    std::vector<int> ch_;
};

// ---------------------------------------------------------------- slide~
// slide~ [up down]: smooths a signal, y += (x - y) / slide. The slide (in
// samples) is `up` while the input is above the output and `down` while it
// is below; 1 (the default) passes the signal through. "reset" jumps to the
// input.
class SlideBox : public Box {
public:
    explicit SlideBox(const Message& a) {
        setPorts({P::Signal, P::Signal, P::Signal}, {P::Signal});
        setScalar(1, argf(a, 0, 1.f));
        setScalar(2, argf(a, 1, 1.f));
    }
    void message(int in, const Message& m) override {
        if (m[0].is("reset")) reset_.store(true);
        else if (m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        const float *x = sigIn(0, n), *up = sigIn(1, n), *down = sigIn(2, n);
        float* out = sigOut(0);
        if (reset_.exchange(false)) y_ = x[0];
        for (int i = 0; i < n; ++i) {
            float s = std::max(1.f, x[i] > y_ ? up[i] : down[i]);
            y_ += (x[i] - y_) / s;
            if (!std::isfinite(y_)) y_ = 0.f;
            out[i] = y_;
        }
    }

private:
    float y_ = 0.f;
    std::atomic<bool> reset_{false};
};

// ---------------------------------------------------------------- selector~ / gate~
// Both switch over 5 ms, so changing the choice never clicks (Max switches
// at once). Each input or output fades between 0 and 1 on its own.
struct Fades {
    std::vector<float> g;
    // moves g[k] one sample towards target; returns the gain to use
    static float step(float& g, float target, float inc) {
        if (g < target) g = std::min(target, g + inc);
        else if (g > target) g = std::max(target, g - inc);
        return g;
    }
};

// selector~ n [initial]: n signal inlets after the left one; the number in
// the left inlet picks the one to pass (1..n, 0 = none).
class SelectorBox : public Box {
public:
    explicit SelectorBox(const Message& a) {
        int n = std::clamp((int)argf(a, 0, 1.f), 1, 64);
        std::vector<Port> in(1, P::Control);
        in.insert(in.end(), (size_t)n, P::Signal);
        setPorts(in, {P::Signal});
        fades_.g.assign((size_t)n, 0.f);
        sel_ = std::clamp((int)argf(a, 1, 0.f), 0, n);
        for (int k = 0; k < n; ++k) fades_.g[(size_t)k] = k + 1 == sel_ ? 1.f : 0.f;
    }
    void message(int in, const Message& m) override {
        if (!m[0].isFloat()) return;
        if (in == 0) sel_.store(std::clamp((int)m[0].f, 0, numIns() - 1));
        else setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        float* out = sigOut(0);
        dsp::fill(out, 0.f, n);
        float inc = (float)(1.0 / (0.005 * patch->root()->sampleRate));
        int sel = sel_.load(std::memory_order_relaxed);
        for (int k = 0; k + 1 < numIns(); ++k) {
            float& g = fades_.g[(size_t)k];
            float target = k + 1 == sel ? 1.f : 0.f;
            if (g == 0.f && target == 0.f) continue;
            const float* x = sigIn(k + 1, n);
            for (int i = 0; i < n; ++i) out[i] += Fades::step(g, target, inc) * x[i];
        }
    }

private:
    Fades fades_;
    std::atomic<int> sel_{0};
};

// gate~ n [initial]: the signal in the right inlet goes to the outlet picked
// by the number in the left inlet (1..n, 0 = closed).
class GateSigBox : public Box {
public:
    explicit GateSigBox(const Message& a) {
        int n = std::clamp((int)argf(a, 0, 1.f), 1, 64);
        setPorts({P::Control, P::Signal}, std::vector<Port>((size_t)n, P::Signal));
        fades_.g.assign((size_t)n, 0.f);
        sel_ = std::clamp((int)argf(a, 1, 0.f), 0, n);
        for (int k = 0; k < n; ++k) fades_.g[(size_t)k] = k + 1 == sel_ ? 1.f : 0.f;
    }
    void message(int in, const Message& m) override {
        if (!m[0].isFloat()) return;
        if (in == 0) sel_.store(std::clamp((int)m[0].f, 0, numOuts()));
        else setScalar(1, m[0].f);
    }
    void dsp(int n) override {
        const float* x = sigIn(1, n);
        float inc = (float)(1.0 / (0.005 * patch->root()->sampleRate));
        int sel = sel_.load(std::memory_order_relaxed);
        for (int k = 0; k < numOuts(); ++k) {
            float* out = sigOut(k);
            float& g = fades_.g[(size_t)k];
            float target = k + 1 == sel ? 1.f : 0.f;
            if (g == 0.f && target == 0.f) { dsp::fill(out, 0.f, n); continue; }
            for (int i = 0; i < n; ++i) out[i] = Fades::step(g, target, inc) * x[i];
        }
    }

private:
    Fades fades_;
    std::atomic<int> sel_{0};
};

// ---------------------------------------------------------------- send~ / receive~
// send~ name: makes its signal available to every receive~ name, in any
// subpatcher; receive~ sums all the send~s of its name. "set name" changes
// the name. The chain runs send~ first, so there is no delay, except around
// a feedback loop through them (one block, 64 samples).
class SendSigBox : public Box {
public:
    explicit SendSigBox(const Message& a) : name_(argstr(a, 0)) { setPorts({P::Signal}, {}); }
    std::string signalSendName() const override { return name_; }
    void resolve() override { buf_.assign((size_t)patch->vectorSize(), 0.f); frames_ = 0; }
    void message(int, const Message& m) override {
        if (m[0].is("set") && m.size() > 1) { name_ = m[1].str(); patch->rebuild(); }
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        int k = std::min(n, (int)buf_.size());
        std::copy(x, x + k, buf_.begin());
        frames_ = k;
    }
    const float* data() const { return buf_.data(); }
    int frames() const { return frames_; }

private:
    std::string name_;
    std::vector<float> buf_;
    int frames_ = 0;
};

class ReceiveSigBox : public Box {
public:
    explicit ReceiveSigBox(const Message& a) : name_(argstr(a, 0)) { setPorts({P::Control}, {P::Signal}); }
    std::string signalReceiveName() const override { return name_; }
    void resolve() override {
        sends_.clear();
        patch->root()->forEachBox([&](Box* b) {
            if (auto s = dynamic_cast<SendSigBox*>(b))
                if (s->signalSendName() == name_) sends_.push_back(s);
        });
    }
    void message(int, const Message& m) override {
        if (m[0].is("set") && m.size() > 1) { name_ = m[1].str(); patch->rebuild(); }
    }
    void dsp(int n) override {
        float* out = sigOut(0);
        dsp::fill(out, 0.f, n);
        for (auto* s : sends_) {
            const float* x = s->data();
            for (int i = 0, k = std::min(n, s->frames()); i < k; ++i) out[i] += x[i];
        }
    }

private:
    std::string name_;
    std::vector<SendSigBox*> sends_;
};

// ---------------------------------------------------------------- dspstate~
// dspstate~ [ms]: reports the audio state on bang, whenever audio starts,
// stops or is reopened, and every ms while audio runs (0 = never). Outlets,
// right to left: signal vector size, I/O buffer size, sample rate, CPU load
// in %, and 1/0 for audio on/off. Sample rate and buffer size are the last
// device settings while audio is off.
class DspStatusBox : public Box {
public:
    explicit DspStatusBox(const Message& a) : interval_(std::max(0.f, argf(a, 0, 0.f))) {
        setPorts({P::Control}, std::vector<Port>(5, P::Control));
    }
    void attached() override { restart(); }
    void audioChanged() override { report(); }
    void message(int, const Message& m) override {
        const Atom* v = m[0].isFloat() ? &m[0] : (m[0].is("interval") && m.size() > 1 ? &m[1] : nullptr);
        if (v && v->isFloat()) { interval_ = std::max(0.f, v->f); restart(); }
        else if (m[0].is("bang")) report();
    }

private:
    void report() {
        Patch* r = patch->root();
        bool on = r->dspRunning;
        // Max's four outlets, then the CPU load
        outletFloat(4, on ? std::round(r->cpuLoad.load() * 1000.f) / 10.f : 0.f);
        outletFloat(3, (float)r->ioBufferFrames.load());
        outletFloat(2, (float)patch->vectorSize());
        outletFloat(1, (float)r->sampleRate.load());
        outletFloat(0, on ? 1.f : 0.f);
    }
    void restart() {
        patch->cancel(this);
        if (interval_ > 0.f) patch->schedule(this, interval_ / 1000.0, [this] { tick(); });
    }
    void tick() {
        if (patch->root()->dspRunning) report();
        patch->schedule(this, std::max(20.f, interval_) / 1000.0, [this] { tick(); });
    }
    float interval_;
};


// ---------------------------------------------------------------- line~
// line~: signal ramp. "target time" pairs make segments ("1 10 0 300");
// a float alone jumps, or ramps over the time given in the right inlet.
class LineSigBox : public Box {
public:
    explicit LineSigBox(const Message& a) {
        setPorts({P::Control, P::Control}, {P::Signal});
        if (!a.empty() && a[0].isFloat()) post({dsp::Ramp::Cmd::Segment, a[0].f, 0.f});
    }
    void message(int in, const Message& m) override {
        if (in == 1) { if (m[0].isFloat()) time_ = m[0].f; return; }
        if (m[0].is("stop")) { post({dsp::Ramp::Cmd::Stop, 0.f, 0.f}); return; }
        if (!m[0].isFloat()) return;
        dropped_ = 0;
        post({dsp::Ramp::Cmd::Begin, 0.f, 0.f});
        int segments = 0;
        if (m.size() == 1) {
            post({dsp::Ramp::Cmd::Segment, m[0].f, time_});
        } else {
            for (size_t k = 0; k < m.size(); k += 2, ++segments) {
                float target = m[k].isFloat() ? m[k].f : 0.f;
                float ms = k + 1 < m.size() && m[k + 1].isFloat() ? m[k + 1].f : 0.f;
                if (segments < dsp::Ramp::kMax) post({dsp::Ramp::Cmd::Segment, target, ms});
                else ++dropped_;
            }
        }
        time_ = 0.f;
        if (dropped_ > 0)       // never silently: more segments than line~ holds, or audio is off
            patch->print("line~: " + std::to_string(dropped_) + " segments dropped (at most " +
                         std::to_string(dsp::Ramp::kMax) + " in a list; is audio on?)");
    }
    void dsp(int n) override { ramp_.process(sigOut(0), n, patch->root()->sampleRate); }

private:
    void post(const dsp::Ramp::Cmd& c) {
        if (!ramp_.post(c)) ++dropped_;
    }
    dsp::Ramp ramp_;
    float time_ = 0.f;
    int dropped_ = 0;
};

// ---------------------------------------------------------------- sig~
class SigBox : public Box {
public:
    explicit SigBox(const Message& a) {
        setPorts({P::Signal}, {P::Signal});
        setScalar(0, argf(a, 0, 0.f));
    }
    void message(int, const Message& m) override { if (m[0].isFloat()) setScalar(0, m[0].f); }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        std::copy(x, x + n, sigOut(0));
    }
};

// ================================================================ oscillators

// ---------------------------------------------------------------- phasor~
class PhasorBox : public Box {
public:
    explicit PhasorBox(const Message& a) {
        setPorts({P::Signal, P::Control}, {P::Signal});
        setScalar(0, argf(a, 0, 0.f));
    }
    void message(int in, const Message& m) override {
        if (!m[0].isFloat()) return;
        if (in == 0) setScalar(0, m[0].f);
        else ph_.setPhase(m[0].f);
    }
    void dsp(int n) override { ph_.process(sigIn(0, n), sigOut(0), n, patch->root()->sampleRate); }

private:
    dsp::Phasor ph_;
};

// ---------------------------------------------------------------- saw~ rect~ tri~
class OscBox : public Box {
public:
    OscBox(dsp::Osc::Shape s, const Message& a) : osc_(s), shape_(s) {
        if (s == dsp::Osc::Saw) setPorts({P::Signal}, {P::Signal});
        else setPorts({P::Signal, P::Signal}, {P::Signal});
        setScalar(0, argf(a, 0, 0.f));
        if (s != dsp::Osc::Saw) setScalar(1, argf(a, 1, 0.5f));
    }
    void message(int in, const Message& m) override { if (m[0].isFloat()) setScalar(in, m[0].f); }
    void dsp(int n) override {
        const float* f = sigIn(0, n);
        const float* d = shape_ == dsp::Osc::Saw ? f : sigIn(1, n);
        osc_.process(f, d, sigOut(0), n, patch->root()->sampleRate);
    }

private:
    dsp::Osc osc_;
    dsp::Osc::Shape shape_;
};

// ================================================================ filters

// ---------------------------------------------------------------- lores~
// lores~ [cf res]: resonant low-pass, res 0..1.
class LoresBox : public Box {
public:
    explicit LoresBox(const Message& a) {
        setPorts({P::Signal, P::Signal, P::Signal}, {P::Signal});
        setScalar(1, argf(a, 0, 1000.f));
        setScalar(2, argf(a, 1, 0.f));
    }
    void message(int in, const Message& m) override {
        if (m[0].is("clear")) { std::lock_guard<std::mutex> lk(patch->mutex()); f_ = dsp::Biquad(); return; }
        if (m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float cf = sigIn(1, n)[0], res = std::clamp(sigIn(2, n)[0], 0.f, 1.f);
        f_.lowpass(cf, 0.707 + res * res * 24.0, patch->root()->sampleRate);
        f_.process(x, sigOut(0), n);
    }

private:
    dsp::Biquad f_;
};

// ---------------------------------------------------------------- svf~
// svf~ [freq q]: state-variable filter; outlets low, high, band, notch. The
// filter of Musil's svf.
class SvfBox : public Box {
public:
    explicit SvfBox(const Message& a) {
        setPorts({P::Signal, P::Signal, P::Signal}, {P::Signal, P::Signal, P::Signal, P::Signal});
        setScalar(1, argf(a, 0, 1000.f));
        setScalar(2, argf(a, 1, 0.5f));
    }
    void message(int in, const Message& m) override {
        if (m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float f = sigIn(1, n)[0], q = sigIn(2, n)[0];
        f_.set(f, q, patch->root()->sampleRate);
        f_.process(x, sigOut(0), sigOut(1), sigOut(2), sigOut(3), n);
    }

private:
    musil::dsp::Svf f_;       // Musil's svf (musil/src/signals/filters.h)
};

// ---------------------------------------------------------------- biquad~
// biquad~ [a0 a1 a2 b1 b2]: y = a0 x + a1 x1 + a2 x2 - b1 y1 - b2 y2.
// Coefficients in inlets 1-5, or as a list in the left inlet.
class BiquadBox : public Box {
public:
    explicit BiquadBox(const Message& a) {
        setPorts({P::Signal, P::Control, P::Control, P::Control, P::Control, P::Control},
                 {P::Signal});
        const float def[5] = {1, 0, 0, 0, 0};
        for (int k = 0; k < 5; ++k) setScalar(1 + k, argf(a, (size_t)k, def[k]));
    }
    void message(int in, const Message& m) override {
        if (in == 0 && m.size() >= 5) {
            for (int k = 0; k < 5; ++k)
                if (m[(size_t)k].isFloat()) setScalar(1 + k, m[(size_t)k].f);
        } else if (m[0].isFloat()) {
            setScalar(in, m[0].f);
        }
    }
    void dsp(int n) override {
        f_.set(scalar(1), scalar(2), scalar(3), scalar(4), scalar(5));
        f_.process(sigIn(0, n), sigOut(0), n);
    }

private:
    dsp::Biquad f_;
};

// ================================================================ signal math

// ---------------------------------------------------------------- clip~ abs~ tanh~
class ClipBox : public Box {
public:
    explicit ClipBox(const Message& a) {
        setPorts({P::Signal, P::Signal, P::Signal}, {P::Signal});
        setScalar(1, argf(a, 0, -1.f));
        setScalar(2, argf(a, 1, 1.f));
    }
    void message(int in, const Message& m) override { if (m[0].isFloat()) setScalar(in, m[0].f); }
    void dsp(int n) override {
        const float *x = sigIn(0, n), *lo = sigIn(1, n), *hi = sigIn(2, n);
        float* y = sigOut(0);
        for (int i = 0; i < n; ++i) y[i] = std::min(std::max(x[i], lo[i]), hi[i]);
    }
};

class UnarySigBox : public Box {
public:
    explicit UnarySigBox(float (*fn)(float)) : fn_(fn) { setPorts({P::Signal}, {P::Signal}); }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float* y = sigOut(0);
        for (int i = 0; i < n; ++i) y[i] = fn_(x[i]);
    }

private:
    float (*fn_)(float);
};

// ================================================================ delays

// ---------------------------------------------------------------- tapin~ / tapout~
// tapin~ maxms writes a delay line; connect its outlet to tapout~, whose
// inlets set each tap's delay (ms). The tap link doesn't order the two, so
// tapout~ -> ... -> tapin~ feedback is allowed (minimum delay: one block).
class TapinBox : public Box {
public:
    explicit TapinBox(const Message& a) {
        setPorts({P::Signal}, {P::Tap});
        maxMs_ = std::clamp(argf(a, 0, 1000.f), 1.f, 60000.f);
    }
    // sized for the rate it runs at, at compile (again when the rate changes), never in dsp()
    void resolve() override {
        double sr = patch->root()->sampleRate.load();
        if (sr == sizedFor_) return;
        sizedFor_ = sr;
        line.resize((size_t)(maxMs_ / 1000.0 * sr) + 4 * dsp::kBlock);
    }
    void message(int, const Message& m) override {
        if (m[0].is("clear")) { std::lock_guard<std::mutex> lk(patch->mutex()); line.clear(); }
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        for (int i = 0; i < n; ++i) line.push(x[i]);
        lastBlock = patch->root()->blockCount;
    }
    float maxMs() const { return maxMs_; }
    dsp::DelayLine line;
    uint64_t lastBlock = ~0ull;

private:
    float maxMs_;
    double sizedFor_ = 0.0;
};

class TapoutBox : public Box {
public:
    explicit TapoutBox(const Message& a) {
        size_t taps = std::max<size_t>(1, a.size());
        setPorts(std::vector<Port>(taps, P::Signal), std::vector<Port>(taps, P::Signal));
        for (size_t k = 0; k < taps; ++k) setScalar((int)k, argf(a, k, 0.f));
    }
    void message(int in, const Message& m) override { if (m[0].isFloat()) setScalar(in, m[0].f); }
    void resolve() override {
        tap_ = nullptr;
        for (auto& c : patch->conns)
            if (c.to == this && c.from->outs[c.outlet] == Port::Tap)
                tap_ = static_cast<TapinBox*>(c.from);
    }
    void dsp(int n) override {
        if (!tap_) {
            for (int k = 0; k < numOuts(); ++k) dsp::fill(sigOut(k), 0.f, n);
            return;
        }
        bool written = tap_->lastBlock == patch->root()->blockCount;   // tapin~ ran this block
        double sr = patch->root()->sampleRate, maxD = tap_->line.size() - 2.0 * dsp::kBlock;
        for (int k = 0; k < numOuts(); ++k) {
            const float* ms = sigIn(k, n);
            float* y = sigOut(k);
            for (int i = 0; i < n; ++i) {
                double d = std::clamp(ms[i] * sr / 1000.0, written ? 0.0 : (double)n, maxD);
                y[i] = tap_->line.read(written ? n - i + d : d - i);
            }
        }
    }

private:
    TapinBox* tap_ = nullptr;
};

// ---------------------------------------------------------------- comb~ / allpass~
// comb~ [maxms delay a b c]: y = a x[n] + b x[n-D] + c y[n-D]
class CombBox : public Box {
public:
    explicit CombBox(const Message& a) {
        setPorts(std::vector<Port>(5, P::Signal), {P::Signal});
        maxMs_ = std::clamp(argf(a, 0, 50.f), 1.f, 10000.f);
        setScalar(1, argf(a, 1, maxMs_ * 0.5f));
        setScalar(2, argf(a, 2, 1.f));
        setScalar(3, argf(a, 3, 0.f));
        setScalar(4, argf(a, 4, 0.5f));
    }
    void message(int in, const Message& m) override {
        if (m[0].is("clear")) { std::lock_guard<std::mutex> lk(patch->mutex()); xs_.clear(); ys_.clear(); return; }
        if (m[0].isFloat()) setScalar(in, m[0].f);
    }
    // sized for the rate it runs at, at compile (again when the rate changes), never in dsp()
    void resolve() override {
        double sr = patch->root()->sampleRate.load();
        if (sr == sizedFor_) return;
        sizedFor_ = sr;
        size_t len = (size_t)(maxMs_ / 1000.0 * sr) + 8;
        xs_.resize(len);
        ys_.resize(len);
        maxD_ = len - 4.0;
    }
    void dsp(int n) override {
        const float *x = sigIn(0, n), *dm = sigIn(1, n), *ga = sigIn(2, n), *gb = sigIn(3, n),
                    *gc = sigIn(4, n);
        float* y = sigOut(0);
        double sr = patch->root()->sampleRate;
        for (int i = 0; i < n; ++i) {
            double D = std::clamp(dm[i] * sr / 1000.0, 1.0, maxD_);
            float out = ga[i] * x[i] + gb[i] * xs_.read(D) + gc[i] * ys_.read(D);
            if (!std::isfinite(out)) out = 0.f;
            xs_.push(x[i]);
            ys_.push(out);
            y[i] = out;
        }
    }

private:
    dsp::DelayLine xs_, ys_;
    double maxD_ = 1.0;
    float maxMs_;
    double sizedFor_ = 0.0;
};

// allpass~ [maxms delay gain]: y = -g x[n] + x[n-D] + g y[n-D]
class AllpassBox : public Box {
public:
    explicit AllpassBox(const Message& a) {
        setPorts({P::Signal, P::Signal, P::Signal}, {P::Signal});
        maxMs_ = std::clamp(argf(a, 0, 50.f), 1.f, 10000.f);
        setScalar(1, argf(a, 1, maxMs_ * 0.5f));
        setScalar(2, argf(a, 2, 0.7f));
    }
    void message(int in, const Message& m) override {
        if (m[0].is("clear")) { std::lock_guard<std::mutex> lk(patch->mutex()); xs_.clear(); ys_.clear(); return; }
        if (m[0].isFloat()) setScalar(in, m[0].f);
    }
    // sized for the rate it runs at, at compile (again when the rate changes), never in dsp()
    void resolve() override {
        double sr = patch->root()->sampleRate.load();
        if (sr == sizedFor_) return;
        sizedFor_ = sr;
        size_t len = (size_t)(maxMs_ / 1000.0 * sr) + 8;
        xs_.resize(len);
        ys_.resize(len);
        maxD_ = len - 4.0;
    }
    void dsp(int n) override {
        const float *x = sigIn(0, n), *dm = sigIn(1, n), *g = sigIn(2, n);
        float* y = sigOut(0);
        double sr = patch->root()->sampleRate;
        for (int i = 0; i < n; ++i) {
            double D = std::clamp(dm[i] * sr / 1000.0, 1.0, maxD_);
            float out = -g[i] * x[i] + xs_.read(D) + g[i] * ys_.read(D);
            if (!std::isfinite(out)) out = 0.f;
            xs_.push(x[i]);
            ys_.push(out);
            y[i] = out;
        }
    }

private:
    dsp::DelayLine xs_, ys_;
    double maxD_ = 1.0;
    float maxMs_;
    double sizedFor_ = 0.0;
};


// ---------------------------------------------------------------- gain~
// gain~ [ramp ms] [amplitude]: a fader that scales the signal in its left
// inlet. Every change ramps linearly over `ramp` ms (20 by default), so
// moving it never clicks. The fader runs in dB from silence to +6 dB; a
// number in the left inlet sets the amplitude (1 = unity), the right inlet
// sets the ramp time. The right outlet reports the amplitude.
class GainBox : public Box {
public:
    explicit GainBox(const Message& a) {
        kind = BoxKind::UI;
        setPorts({P::Signal, P::Control}, {P::Signal, P::Control});
        ramp_.store(std::max(0.f, argf(a, 0, 20.f)));
        amp_ = std::clamp(argf(a, 1, 1.f), 0.f, 2.f);
        target_.store(amp_);
        cur_ = goal_ = amp_;
    }
    void uiSize(int& w, int& h) const override { w = 38; h = 132; }
    Message saveArgs() const override { return {Atom(ramp_.load()), Atom(amp_)}; }
    void message(int in, const Message& m) override {
        if (!m[0].isFloat()) {
            if (m[0].is("bang")) outletFloat(1, amp_);
            return;
        }
        if (in == 1) ramp_.store(std::max(0.f, m[0].f));
        else set(m[0].f, false);
    }
    void uiMouse(int e, int, int my) override {
        if (e == FL_RELEASE) return;
        double p = 1.0 - (my - 10.0) / (h - 30.0);
        set(ampAt(std::clamp(p, 0.0, 1.0)), true);
    }
    void drawUI(int X, int Y) override {
        int tx = X + w / 2, top = Y + 10, len = h - 30;
        double p = posOf(amp_);
        fl_color(fl_rgb_color(30, 31, 36));
        zd::rounded_rectf(tx - 3, top, 6, len, 3);
        int ky = top + (int)((1.0 - p) * len);
        fl_color(kSignal);
        zd::rounded_rectf(tx - 3, ky, 6, top + len - ky, 3);
        int uy = top + (int)((1.0 - posOf(1.0)) * len);       // unity mark
        fl_color(kDim);
        zd::line(X + 4, uy, X + 9, uy);
        zd::line(X + w - 10, uy, X + w - 5, uy);
        fl_color(kText);
        zd::rounded_rectf(X + 6, ky - 4, w - 12, 8, 3);
        char buf[16];
        if (amp_ <= 0.f) std::snprintf(buf, sizeof buf, "-inf");
        else std::snprintf(buf, sizeof buf, "%.1f", 20.0 * std::log10(amp_));
        zd::font(kFont, 10);
        fl_color(kDim);
        zd::draw(buf, X, Y + h - 16, w, 14, FL_ALIGN_CENTER);
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float* y = sigOut(0);
        float tgt = target_.load(std::memory_order_relaxed);
        if (tgt != goal_) {                       // start a new ramp from where we are
            goal_ = tgt;
            left_ = std::max(1L, (long)(ramp_.load() * patch->root()->sampleRate / 1000.0));
            inc_ = (goal_ - cur_) / (double)left_;
        }
        for (int i = 0; i < n; ++i) {
            if (left_ > 0) {
                cur_ += inc_;
                if (--left_ == 0) cur_ = goal_;
            }
            y[i] = x[i] * (float)cur_;
        }
    }

private:
    // fader position 0..1 <-> amplitude: the bottom 2% is silence, then dB
    static double posOf(double a) {
        if (a <= 0.0) return 0.0;
        double db = std::clamp(20.0 * std::log10(a), -70.0, 6.0);
        return 0.02 + 0.98 * (db + 70.0) / 76.0;
    }
    static float ampAt(double p) {
        if (p <= 0.02) return 0.f;
        return (float)std::pow(10.0, (-70.0 + 76.0 * (p - 0.02) / 0.98) / 20.0);
    }
    void set(float a, bool out) {
        a = std::clamp(a, 0.f, 2.f);
        bool changed = a != amp_;
        amp_ = a;
        target_.store(a);
        if (patch) patch->redraw();
        if (out && changed) outletFloat(1, a);
    }
    float amp_ = 1.f;                              // GUI side
    std::atomic<float> target_{1.f}, ramp_{20.f};
    double cur_ = 1.0, goal_ = 1.0, inc_ = 0.0;     // audio side
    long left_ = 0;
};

}  // namespace

void registerSignalObjects(ClassTable& t) {
    using M = const Message&;
    auto sigop = [](dsp::Op op) -> Factory {
        return [op](M a, Patch*) { return std::make_unique<SigOpBox>(op, a); };
    };
    auto osc = [](dsp::Osc::Shape s) -> Factory {
        return [s](M a, Patch*) { return std::make_unique<OscBox>(s, a); };
    };
    auto unary = [](float (*f)(float)) -> Factory {
        return [f](M, Patch*) { return std::make_unique<UnarySigBox>(f); };
    };
    t.insert(t.end(), {
        {"cycle~", "Oscillators", "cosine oscillator", with<CycleBox>()},
        {"phasor~", "Oscillators", "ramp 0..1", with<PhasorBox>()},
        {"saw~", "Oscillators", "band-limited sawtooth", osc(dsp::Osc::Saw)},
        {"rect~", "Oscillators", "band-limited pulse (freq, duty)", osc(dsp::Osc::Rect)},
        {"tri~", "Oscillators", "triangle (freq, slope)", osc(dsp::Osc::Tri)},
        {"noise~", "Oscillators", "white noise", plain<NoiseBox>()},
        {"click~", "Oscillators", "impulse on bang", with<ClickBox>()},
        {"sig~", "Oscillators", "number to signal", with<SigBox>()},
        {"line~", "Oscillators", "signal ramps: target time ...", with<LineSigBox>()},
        {"reson~", "Filters", "resonant bandpass filter", with<ResonBox>()},
        {"lores~", "Filters", "resonant low-pass (cf, res 0-1)", with<LoresBox>()},
        {"svf~", "Filters", "state-variable: low high band notch", with<SvfBox>()},
        {"biquad~", "Filters", "two-pole two-zero filter", with<BiquadBox>()},
        {"+~", "Signal Math", "add signals", sigop(dsp::Op::Add)},
        {"-~", "Signal Math", "subtract signals", sigop(dsp::Op::Sub)},
        {"*~", "Signal Math", "multiply signals", sigop(dsp::Op::Mul)},
        {"/~", "Signal Math", "divide signals", sigop(dsp::Op::Div)},
        {"gain~", "Signal Math", "click-free gain fader [ramp ms]", with<GainBox>()},
        {"clip~", "Signal Math", "limit a signal (min, max)", with<ClipBox>()},
        {"abs~", "Signal Math", "absolute value", unary([](float x) { return std::fabs(x); })},
        {"tanh~", "Signal Math", "soft saturation", unary([](float x) { return std::tanh(x); })},
        {"tapin~", "Delay", "delay line input (max ms)", with<TapinBox>()},
        {"tapout~", "Delay", "delay line taps (ms ...)", with<TapoutBox>()},
        {"comb~", "Delay", "comb filter (max, delay, a, b, c)", with<CombBox>()},
        {"allpass~", "Delay", "allpass filter (max, delay, gain)", with<AllpassBox>()},
        {"adc~", "I/O", "audio input", with<AdcBox>()},
        {"dac~", "I/O", "audio output", with<DacBox>()},
        {"slide~", "Filters", "smooth a signal: slide up, slide down (samples)", with<SlideBox>()},
        {"selector~", "Signal Routing", "pass one of n signals: n [initial]", with<SelectorBox>()},
        {"gate~", "Signal Routing", "send a signal to one of n outlets: n [initial]", with<GateSigBox>()},
        {"send~", "Signal Routing", "send a signal by name", with<SendSigBox>()},
        {"receive~", "Signal Routing", "receive signals sent by name", with<ReceiveSigBox>()},
        {"dspstate~", "I/O", "audio state: on, sample rate, vector sizes, CPU [ms]", with<DspStatusBox>()},
    });
}

}  // namespace moritz
