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
        else post(kPhase, m[0].f);                       // at its sample
    }
    void dsp(int n) override {
        const float* f = sigIn(0, n);
        float* y = sigOut(0);
        double sr = patch->root()->sampleRate;
        eventPieces(*this, n, [&](int a, int b) { osc_.process(f + a, y + a, b - a, sr); },
                    [&](const Event& e) { osc_.setPhase(e.a); });
    }

private:
    enum : uint8_t { kPhase = kEvUser };
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
        if (m[0].is("clear")) { post(kClear); return; }   // the audio thread clears it
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
        if (events() && hasEvent(kClear)) f_.clear();
        f_.process(x, sigOut(0), n, g, cf, q, patch->root()->sampleRate);
    }

private:
    enum : uint8_t { kClear = kEvUser };
    bool hasEvent(uint8_t kind) const {
        for (int k = 0; k < events(); ++k)
            if (event(k).kind == kind) return true;
        return false;
    }
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
// click~: bang outputs an impulse (at the sample the bang is for); a list of
// floats ("set" optional) sets the impulse shape.
class ClickBox : public Box {
public:
    explicit ClickBox(const Message& a) {
        setPorts({P::Control}, {P::Signal});
        if (!a.empty()) setShape(a);
    }
    void message(int, const Message& m) override {
        if (m[0].is("bang")) { post(kBang); return; }
        setShape(m[0].is("set") ? Message(m.begin() + 1, m.end()) : m);
    }
    void dsp(int n) override {
        float* y = sigOut(0);
        int ev = 0, ne = events();
        for (int i = 0; i < n; ++i) {
            for (; ev < ne && eventOffset(ev) <= i; ++ev) {
                if (event(ev).kind == kBang) pos_ = 0;
                else if (event(ev).kind == kShape) { shapes_.take(); pos_ = SIZE_MAX; }
            }
            const std::vector<float>* sh = shapes_.current();
            const float* v = sh ? sh->data() : &one_;
            size_t len = sh ? sh->size() : 1;
            y[i] = pos_ < len ? v[pos_++] : 0.f;
        }
    }

private:
    enum : uint8_t { kBang = kEvUser, kShape };
    // the shape goes to the audio thread through a mailbox (the old one comes back to be freed)
    void setShape(const Message& m) {
        auto s = std::make_unique<std::vector<float>>();
        for (auto& a : m) if (a.isFloat()) s->push_back(a.f);
        if (s->empty()) s->push_back(1.f);
        if (shapes_.send(std::move(s))) post(kShape);
    }
    Mailbox<std::vector<float>, 16> shapes_;
    const float one_ = 1.f;
    size_t pos_ = SIZE_MAX;                    // audio side
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
        if (m[0].is("reset")) post(kReset);
        else if (m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        const float *x = sigIn(0, n), *up = sigIn(1, n), *down = sigIn(2, n);
        float* out = sigOut(0);
        int ev = 0, ne = events();
        for (int i = 0; i < n; ++i) {
            for (; ev < ne && eventOffset(ev) <= i; ++ev)
                if (event(ev).kind == kReset) y_ = x[i];      // jumps to the input, at its sample
            float s = std::max(1.f, x[i] > y_ ? up[i] : down[i]);
            y_ += (x[i] - y_) / s;
            if (!std::isfinite(y_)) y_ = 0.f;
            out[i] = y_;
        }
    }

private:
    enum : uint8_t { kReset = kEvUser };
    float y_ = 0.f;
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
    // its buffer holds the largest vector there is (a pfft~ frame): never resized while it runs
    explicit SendSigBox(const Message& a) : name_(argstr(a, 0)), buf_(16384, 0.f) { setPorts({P::Signal}, {}); }
    std::string signalSendName() const override { return name_; }
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
    // the send~s of its name, at compile: a new list is published (the old one retired)
    void resolve() override {
        std::vector<SendSigBox*> found;
        patch->root()->forEachBox([&](Box* b) {
            if (auto s = dynamic_cast<SendSigBox*>(b))
                if (s->signalSendName() == name_) found.push_back(s);
        });
        if (sends_.get() && *sends_.get() == found) return;
        sends_.set(patch, std::make_unique<std::vector<SendSigBox*>>(std::move(found)));
    }
    void message(int, const Message& m) override {
        if (m[0].is("set") && m.size() > 1) { name_ = m[1].str(); patch->rebuild(); }
    }
    void dsp(int n) override {
        float* out = sigOut(0);
        dsp::fill(out, 0.f, n);
        const std::vector<SendSigBox*>* sends = sends_.get();
        if (!sends) return;
        for (auto* s : *sends) {
            const float* x = s->data();
            for (int i = 0, k = std::min(n, s->frames()); i < k; ++i) out[i] += x[i];
        }
    }

private:
    std::string name_;
    Published<std::vector<SendSigBox*>> sends_;
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
// a float alone jumps, or ramps over the time given in the right inlet. A
// list starts at the sample the message is for; its length has no limit
// (the list goes to the audio thread whole, through a mailbox). Fed
// directly into play~, its value reaches it in double precision.
class LineSigBox : public Box {
public:
    using Seg = dsp::Ramp::Seg;
    explicit LineSigBox(const Message& a) {
        setPorts({P::Control, P::Control}, {P::Signal});
        if (!a.empty() && a[0].isFloat()) send({Seg{a[0].f, 0.f}});
    }
    bool preciseOut(int) const override { return true; }
    void message(int in, const Message& m) override {
        if (in == 1) { if (m[0].isFloat()) time_ = m[0].f; return; }
        if (m[0].is("stop")) { post(kStop); return; }
        if (!m[0].isFloat()) return;
        std::vector<Seg> list;
        if (m.size() == 1) {
            list.push_back({m[0].f, time_});
        } else {
            for (size_t k = 0; k < m.size(); k += 2) {
                float target = m[k].isFloat() ? m[k].f : 0.f;
                float ms = k + 1 < m.size() && m[k + 1].isFloat() ? m[k + 1].f : 0.f;
                list.push_back({target, ms});
            }
        }
        time_ = 0.f;
        send(std::move(list));
    }
    void dsp(int n) override {
        float* y = sigOut(0);
        double* yd = sigOutPrecise(0);
        double sr = patch->root()->sampleRate;
        eventPieces(*this, n, [&](int a, int b) { ramp_.process(y + a, yd ? yd + a : nullptr, b - a, sr); },
                    [&](const Event& e) {
                        if (e.kind == kStop) ramp_.stop();
                        else if (e.kind == kList) {
                            ramp_.settle(sr);     // the old list's leading jumps first: take() hands it back to be freed
                            if (const std::vector<Seg>* l = lists_.take()) ramp_.start(l->data(), (int)l->size());
                        }
                    });
    }

private:
    enum : uint8_t { kList = kEvUser, kStop };
    void send(std::vector<Seg> list) {
        if (lists_.send(std::make_unique<std::vector<Seg>>(std::move(list)))) post(kList);
        else if (patch)       // never silently: the audio thread has not taken the ones before
            patch->print("line~: a ramp dropped: " + std::to_string(kLists - 1) + " are waiting already (is audio on?)");
    }
    static constexpr size_t kLists = 64;
    Mailbox<std::vector<Seg>, kLists> lists_;
    dsp::Ramp ramp_;
    float time_ = 0.f;
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
        else post(kPhase, m[0].f);                       // at its sample
    }
    void dsp(int n) override {
        const float* f = sigIn(0, n);
        float* y = sigOut(0);
        double sr = patch->root()->sampleRate;
        eventPieces(*this, n, [&](int a, int b) { ph_.process(f + a, y + a, b - a, sr); },
                    [&](const Event& e) { ph_.setPhase(e.a); });
    }

private:
    enum : uint8_t { kPhase = kEvUser };
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
        if (m[0].is("clear")) { post(kClear); return; }  // the audio thread clears it
        if (m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        for (int k = 0; k < events(); ++k)
            if (event(k).kind == kClear) f_ = dsp::Biquad();
        float cf = sigIn(1, n)[0], res = std::clamp(sigIn(2, n)[0], 0.f, 1.f);
        f_.lowpass(cf, 0.707 + res * res * 24.0, patch->root()->sampleRate);
        f_.process(x, sigOut(0), n);
    }

private:
    enum : uint8_t { kClear = kEvUser };
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
// The line is sized for the rate the patch runs at, made at compile (again
// when the rate changes) and published; `clear` empties it on the audio thread.
class TapinBox : public Box {
public:
    explicit TapinBox(const Message& a) {
        setPorts({P::Signal}, {P::Tap});
        maxMs_ = std::clamp(argf(a, 0, 1000.f), 1.f, 60000.f);
    }
    void resolve() override {
        double sr = patch->root()->sampleRate.load();
        if (sr == sizedFor_) return;
        sizedFor_ = sr;
        auto l = std::make_unique<dsp::DelayLine>();
        l->resize((size_t)(maxMs_ / 1000.0 * sr) + 4 * dsp::kBlock);
        line_.set(patch, std::move(l));
    }
    void message(int, const Message& m) override {
        if (m[0].is("clear")) post(kClear);
    }
    void dsp(int n) override {
        dsp::DelayLine* l = line_.get();
        if (!l) return;
        if (events())
            for (int k = 0; k < events(); ++k)
                if (event(k).kind == kClear) l->clear();
        const float* x = sigIn(0, n);
        for (int i = 0; i < n; ++i) l->push(x[i]);
        lastBlock = patch->root()->blockCount;
    }
    float maxMs() const { return maxMs_; }
    dsp::DelayLine* line() const { return line_.get(); }   // audio thread: for this block
    uint64_t lastBlock = ~0ull;                            // audio side

private:
    enum : uint8_t { kClear = kEvUser };
    float maxMs_;
    double sizedFor_ = 0.0;
    Published<dsp::DelayLine> line_;
};

class TapoutBox : public Box {
public:
    explicit TapoutBox(const Message& a) {
        size_t taps = std::max<size_t>(1, a.size());
        setPorts(std::vector<Port>(taps, P::Signal), std::vector<Port>(taps, P::Signal));
        for (size_t k = 0; k < taps; ++k) setScalar((int)k, argf(a, k, 0.f));
    }
    bool acceptsTap(int inlet) const override { return inlet == 0; }
    void message(int in, const Message& m) override { if (m[0].isFloat()) setScalar(in, m[0].f); }
    void resolve() override {
        TapinBox* t = nullptr;
        for (auto& c : patch->conns)
            if (c.to == this && c.from->outs[c.outlet] == Port::Tap) t = static_cast<TapinBox*>(c.from);
        tap_.store(t);
    }
    void dsp(int n) override {
        TapinBox* tap = tap_.load(std::memory_order_relaxed);
        dsp::DelayLine* line = tap ? tap->line() : nullptr;
        if (!line) {
            for (int k = 0; k < numOuts(); ++k) dsp::fill(sigOut(k), 0.f, n);
            return;
        }
        bool written = tap->lastBlock == patch->root()->blockCount;   // tapin~ ran this block
        double sr = patch->root()->sampleRate, maxD = line->size() - 2.0 * dsp::kBlock;
        for (int k = 0; k < numOuts(); ++k) {
            const float* ms = sigIn(k, n);
            float* y = sigOut(k);
            for (int i = 0; i < n; ++i) {
                double d = std::clamp(ms[i] * sr / 1000.0, written ? 0.0 : (double)n, maxD);
                y[i] = line->read(written ? n - i + d : d - i);
            }
        }
    }

private:
    std::atomic<TapinBox*> tap_{nullptr};      // at compile; the audio thread reads it
};

// ---------------------------------------------------------------- comb~ / allpass~
// The delay lines of both: sized for the rate the patch runs at, made at
// compile (again when the rate changes) and published; `clear` empties them
// on the audio thread.
struct CombLines {
    dsp::DelayLine xs, ys;
    double maxD = 1.0;
};

class CombLike : public Box {
public:
    void message(int in, const Message& m) override {
        if (m[0].is("clear")) { post(kClear); return; }
        if (m[0].isFloat()) setScalar(in, m[0].f);
    }
    void resolve() override {
        double sr = patch->root()->sampleRate.load();
        if (sr == sizedFor_) return;
        sizedFor_ = sr;
        size_t len = (size_t)(maxMs_ / 1000.0 * sr) + 8;
        auto l = std::make_unique<CombLines>();
        l->xs.resize(len);
        l->ys.resize(len);
        l->maxD = len - 4.0;
        lines_.set(patch, std::move(l));
    }

protected:
    CombLines* lines() {                       // audio thread: for this block, cleared if asked
        CombLines* l = lines_.get();
        if (l)
            for (int k = 0; k < events(); ++k)
                if (event(k).kind == kClear) { l->xs.clear(); l->ys.clear(); }
        return l;
    }
    float maxMs_ = 50.f;

private:
    enum : uint8_t { kClear = kEvUser };
    double sizedFor_ = 0.0;
    Published<CombLines> lines_;
};

// comb~ [maxms delay a b c]: y = a x[n] + b x[n-D] + c y[n-D]
class CombBox : public CombLike {
public:
    explicit CombBox(const Message& a) {
        setPorts(std::vector<Port>(5, P::Signal), {P::Signal});
        maxMs_ = std::clamp(argf(a, 0, 50.f), 1.f, 10000.f);
        setScalar(1, argf(a, 1, maxMs_ * 0.5f));
        setScalar(2, argf(a, 2, 1.f));
        setScalar(3, argf(a, 3, 0.f));
        setScalar(4, argf(a, 4, 0.5f));
    }
    void dsp(int n) override {
        float* y = sigOut(0);
        CombLines* l = lines();
        if (!l) { dsp::fill(y, 0.f, n); return; }
        const float *x = sigIn(0, n), *dm = sigIn(1, n), *ga = sigIn(2, n), *gb = sigIn(3, n),
                    *gc = sigIn(4, n);
        double sr = patch->root()->sampleRate;
        for (int i = 0; i < n; ++i) {
            double D = std::clamp(dm[i] * sr / 1000.0, 1.0, l->maxD);
            float out = ga[i] * x[i] + gb[i] * l->xs.read(D) + gc[i] * l->ys.read(D);
            if (!std::isfinite(out)) out = 0.f;
            l->xs.push(x[i]);
            l->ys.push(out);
            y[i] = out;
        }
    }
};

// allpass~ [maxms delay gain]: y = -g x[n] + x[n-D] + g y[n-D]
class AllpassBox : public CombLike {
public:
    explicit AllpassBox(const Message& a) {
        setPorts({P::Signal, P::Signal, P::Signal}, {P::Signal});
        maxMs_ = std::clamp(argf(a, 0, 50.f), 1.f, 10000.f);
        setScalar(1, argf(a, 1, maxMs_ * 0.5f));
        setScalar(2, argf(a, 2, 0.7f));
    }
    void dsp(int n) override {
        float* y = sigOut(0);
        CombLines* l = lines();
        if (!l) { dsp::fill(y, 0.f, n); return; }
        const float *x = sigIn(0, n), *dm = sigIn(1, n), *g = sigIn(2, n);
        double sr = patch->root()->sampleRate;
        for (int i = 0; i < n; ++i) {
            double D = std::clamp(dm[i] * sr / 1000.0, 1.0, l->maxD);
            float out = -g[i] * x[i] + l->xs.read(D) + g[i] * l->ys.read(D);
            if (!std::isfinite(out)) out = 0.f;
            l->xs.push(x[i]);
            l->ys.push(out);
            y[i] = out;
        }
    }
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
