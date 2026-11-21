// Copyright (c) 2024-2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_sound.cpp — sound processing from the author's tools: sparkle~
// (sparkle's phase vocoder), conv~ (pconv/condec convolution), condec
// (convolution and deconvolution of buffers), function (a breakpoint
// editor), nebula~ (sound_math's granular synthesis) and harmonize~ (reVoice's
// harmonizer). descriptors~ is with the live analyses, in objects_mir.cpp.

#include "objects.h"
#include "zoom.h"
#include "signals/convolution.h"   // Musil's convolution engines
#include "signals/vocoder.h"   // Musil's phase vocoder engine, as its pvoc
#include "signals/nebula.h"    // Musil's granular engine, as its nebula
#include "signals/psola.h"     // Musil's harmonizer, as its harmonize

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <mutex>
#include <cstdio>

namespace moritz {
namespace {

// ---------------------------------------------------------------- sparkle~
// sparkle~ [buffer] [channels] [window] [overlap]: sparkle's phase vocoder,
// the engine of Musil's pvoc (musil/src/signals/vocoder.h).
// With a buffer~ name it plays the buffer: the left inlet is the speed (1 =
// as recorded, 0.25 = four times slower, 0 = frozen, negative = backwards),
// `start [ms]` or bang plays, `stop` stops, `loop 1` loops. Without a name it
// transforms the signal in its left inlet. A frame is computed on a worker
// thread of its own while the next hop plays (rendering offline: at once),
// so no audio block carries a whole frame; its output is heard from the hop
// after: the latency is one window and one hop. A frame the worker has not
// finished in time is skipped (a hop with less sound), never waited for.
// Inlets 2 and 3: pitch and formant ratios (signals or numbers, read once per
// frame). `envelope C` preserves the spectral envelope with C cepstral
// coefficients (0 = off; 30-80 for voices), so a transposed voice keeps its
// formants; `threshold x` drops bins weaker than x times a full-scale sine
// (denoising: 0.001 is -60 dB); `robot 1` and
// `whisper 1` replace the phases. Outlets: the channels, then the read
// position, 0 to 1 (buffer mode).
// Cross-synthesis with a second sound, as in sparkle: `cross mode [buffer]`,
// mode 1 multiplicative, 2 spectral flattener (the second sound's envelope on
// the first, with the `envelope` order, 40 if none), 3 morphing, 0 off. The
// second sound is the buffer~ named (read at the same position, or on its
// own when the first is live), or else the signal in inlet 5. Inlet 4 is the
// amount, 0 to 1.
class SparkleBox : public Box {
public:
    explicit SparkleBox(const Message& a) {
        size_t k = 0;
        if (k < a.size() && a[k].isSymbol()) ref_.set(nullptr, a[k++].s);
        live_ = ref_.name().empty();
        ch_ = live_ ? 1 : std::clamp((int)argf(a, k++, 1.f), 1, 8);
        wn_ = std::clamp((int)argf(a, k++, 2048.f), 64, 16384);
        int olap = std::clamp((int)argf(a, k++, 8.f), 2, 32);
        hop_ = std::max(1, wn_ / olap);
        for (int c = 0; c < ch_; ++c) {
            chans_.push_back(std::make_unique<Chan>());
            Chan& x = *chans_.back();
            x.voc = std::make_unique<musil::dsp::Vocoder>(wn_, 1);    // sparkle's default padding (f = 1)
            x.voc->seed(1234u + (unsigned)c);
            for (auto* v : {&x.now, &x.prev, &x.oNow, &x.oPrev, &x.out}) v->assign((size_t)wn_, 0.f);
            x.ola.assign((size_t)wn_ + hop_, 0.f);
        }
        if (live_) ring_.assign((size_t)(wn_ + hop_) * 2, 0.f);
        ring2_.assign((size_t)(wn_ + hop_) * 2, 0.f);
        std::vector<Port> ins{P::Signal, P::Signal, P::Signal, P::Signal, P::Signal};
        std::vector<Port> outs((size_t)ch_, P::Signal);
        if (!live_) outs.push_back(P::Signal);
        setPorts(ins, outs);
        setScalar(0, live_ ? 0.f : 1.f);   // speed 1
        setScalar(1, 1.f);                  // pitch
        setScalar(2, 1.f);                  // formants
        setScalar(3, 1.f);                  // cross amount: the second sound fully
        ready_ = 0;
    }
    ~SparkleBox() override {
        if (worker_.joinable()) {
            quit_.store(true);
            wake_.notify_one();
            worker_.join();
        }
    }
    // the worker thread that computes the frames while audio plays (not when rendering offline)
    void attached() override {
        if (patch->root()->offline || worker_.joinable()) return;
        worker_ = std::thread([this] { work(); });
        async_.store(true, std::memory_order_release);
    }
    void resolve() override {
        if (!live_) ref_.resolve(patch);
        xref_.resolve(patch);
    }
    void message(int in, const Message& m) override {
        if (in > 0) { if (m[0].isFloat()) setScalar(in, m[0].f); return; }
        if (m[0].isFloat()) { setScalar(0, m[0].f); return; }
        if (m[0].is("bang")) post(kStart, 0.f);
        else if (m[0].is("start")) post(kStart, std::max(0.f, argf(m, 1, 0.f)));
        else if (m[0].is("stop")) post(kStop);
        else if (m[0].is("loop")) loop_.store(argf(m, 1, 1.f) != 0.f);
        else if (m[0].is("envelope")) cepstrum_.store(std::clamp((int)argf(m, 1, 0.f), 0, wn_));
        else if (m[0].is("threshold")) threshold_.store(std::max(0.f, argf(m, 1, 0.f)));
        else if (m[0].is("robot")) phaseMode_.store(argf(m, 1, 1.f) != 0.f ? 1 : 0);
        else if (m[0].is("whisper")) phaseMode_.store(argf(m, 1, 1.f) != 0.f ? 2 : 0);
        else if (m[0].is("set") && m.size() > 1 && !live_) ref_.set(patch, m[1].str());   // nothing compiles
        else if (m[0].is("cross")) {
            crossMode_.store(std::clamp((int)argf(m, 1, 0.f), 0, 3));
            xref_.set(patch, m.size() > 2 ? m[2].str() : "");
        }
    }
    void dsp(int n) override {
        const float *spd = sigIn(0, n), *pitch = sigIn(1, n), *form = sigIn(2, n);
        const float *amount = sigIn(3, n), *other = sigIn(4, n);
        const SampleBuffer* sb = live_ ? nullptr : ref_.get();
        for (int k = 0; k < events(); ++k) {               // start and stop (a frame is the grain)
            const Event& e = event(k);
            if (e.kind == kStart && sb) {
                pos_ = e.a / 1000.0 * sb->sr;
                playing_ = true;
                for (auto& c : chans_) std::fill(c->ola.begin(), c->ola.end(), 0.f);
                reset_ = true;                              // the next frame resets the vocoders
                ++gen_;                                     // a frame computing now is not heard
                ready_ = 0;
            } else if (e.kind == kStop) {
                playing_ = false;
            }
        }
        for (int i = 0; i < n; ++i) {
            if (live_) ring_[(size_t)ringPos_] = spd[i];        // keep the input(s)
            ring2_[(size_t)ringPos_] = other[i];
            ringPos_ = (ringPos_ + 1) % (int)ring2_.size();
            if (ready_ == 0) {
                hop(pitch[i], form[i], spd[i], amount[i]);
                ready_ = hop_;
                readPos_ = 0;
            }
            for (int c = 0; c < ch_; ++c) sigOut(c)[i] = chans_[(size_t)c]->ola[(size_t)readPos_];
            if (!live_) sigOut(ch_)[i] = sync_;
            ++readPos_;
            if (--ready_ == 0)                                // the hop is out: shift the overlap-add
                for (auto& c : chans_) {
                    std::copy(c->ola.begin() + hop_, c->ola.end(), c->ola.begin());
                    std::fill(c->ola.end() - hop_, c->ola.end(), 0.f);
                }
        }
    }

private:
    struct Chan {
        std::unique_ptr<musil::dsp::Vocoder> voc;
        std::vector<float> now, prev, out, ola, oNow, oPrev;
    };
    // a frame's settings, taken with its input
    struct Job {
        float P = 1.f, F = 1.f, thr = 0.f, amount = 0.f;
        int C = 0, D = 0, xmode = 0, phaseMode = 0;
        bool reset = false;
        unsigned gen = 0;
    };
    enum { kIdle, kBusy, kDone };

public:
    bool working() const override { return state_.load(std::memory_order_acquire) == kBusy; }

private:
    // At each hop: the frame computed during the last hop is overlap-added (heard from now on),
    // and the next frame's input is taken and handed to the worker (or computed at once offline).
    void hop(float pitch, float formant, float speed, float amount) {
        int st = state_.load(std::memory_order_acquire);
        if (st == kBusy) { ++late_; return; }              // the worker is late: this hop has no new frame
        if (st == kDone && job_.gen == gen_)
            for (auto& c : chans_)
                for (int k = 0; k < wn_; ++k) c->ola[(size_t)k] += c->out[(size_t)k];
        state_.store(kIdle, std::memory_order_relaxed);
        if (!capture(pitch, formant, speed, amount)) return;
        if (async_.load(std::memory_order_acquire)) {
            state_.store(kBusy, std::memory_order_release);
            wake_.notify_one();                           // no lock taken: the worker also looks every 2 ms
        } else {
            compute();
            state_.store(kDone, std::memory_order_release);
        }
    }
    // the worker thread
    void work() {
        while (!quit_.load()) {
            {
                std::unique_lock<std::mutex> lk(wakeMutex_);
                wake_.wait_for(lk, std::chrono::milliseconds(2),
                               [&] { return quit_.load() || state_.load(std::memory_order_acquire) == kBusy; });
            }
            if (state_.load(std::memory_order_acquire) == kBusy) {
                compute();
                state_.store(kDone, std::memory_order_release);
            }
        }
    }
    // one analysis/synthesis frame of every channel into its out (the worker, or offline the audio thread)
    void compute() {
        const Job& j = job_;
        for (auto& cp : chans_) {
            Chan& x = *cp;
            if (j.reset) x.voc->reset();
            x.voc->phaseMode = j.phaseMode;
            x.voc->crossMode = j.xmode;
            x.voc->envelope = j.C;
            x.voc->process(x.now.data(), x.prev.data(), j.xmode ? x.oNow.data() : nullptr, j.xmode ? x.oPrev.data() : nullptr,
                           (double)j.D, (double)hop_, j.P, j.F, j.thr, j.amount, x.out.data());
        }
    }
    // the input of the next frame (false: nothing plays), and the read position moved on
    bool capture(float pitch, float formant, float speed, float amount) {
        const SampleBuffer* sb = live_ ? nullptr : ref_.get();
        bool active = live_ || (playing_ && sb && sb->frames > wn_);
        if (!active) { sync_ = 0.f; return false; }
        double ratio = live_ ? 1.0 : sb->sr / patch->root()->sampleRate.load();
        float P = std::clamp(pitch, 0.05f, 8.f) * (float)ratio;
        float F = std::clamp(formant, 0.05f, 8.f) * (float)ratio;
        int C = cepstrum_.load(), D = hop_;
        float thr = threshold_.load();
        int xmode = crossMode_.load();
        const SampleBuffer* xb = xref_.get();
        if (xb && xb->frames <= 0) xb = nullptr;
        // the second sound's read position: with the first one's when that
        // is a buffer, else its own, moving on in real time
        long xstart = !live_ ? (long)std::floor(pos_) : (long)std::floor(xpos_);
        for (int c = 0; c < ch_; ++c) {
            Chan& x = *chans_[(size_t)c];
            long start = live_ ? 0 : (long)std::floor(pos_);
            for (int k = 0; k < wn_; ++k) {
                if (live_) {                              // the newest wn_ samples, and wn_ ending D earlier
                    int L = (int)ring_.size();
                    x.now[(size_t)k] = ring_[(size_t)((ringPos_ - wn_ + k + 2 * L) % L)];
                    x.prev[(size_t)k] = ring_[(size_t)((ringPos_ - wn_ - D + k + 2 * L) % L)];
                } else {
                    x.now[(size_t)k] = sample(*sb, start + k, c);
                    x.prev[(size_t)k] = sample(*sb, start - D + k, c);
                }
            }
            if (xmode) {
                int L = (int)ring2_.size();
                for (int k = 0; k < wn_; ++k) {
                    if (xb) {
                        auto at = [&](long f) { f = ((f % xb->frames) + xb->frames) % xb->frames; return xb->at(f, c % xb->channels); };
                        x.oNow[(size_t)k] = at(xstart + k);
                        x.oPrev[(size_t)k] = at(xstart - D + k);
                    } else {
                        x.oNow[(size_t)k] = ring2_[(size_t)((ringPos_ - wn_ + k + 2 * L) % L)];
                        x.oPrev[(size_t)k] = ring2_[(size_t)((ringPos_ - wn_ - D + k + 2 * L) % L)];
                    }
                }
            }
        }
        job_.P = P;
        job_.F = F;
        job_.thr = thr;
        job_.amount = amount;
        job_.C = C;
        job_.D = D;
        job_.xmode = xmode;
        job_.phaseMode = phaseMode_.load();
        job_.reset = reset_;
        job_.gen = gen_;
        reset_ = false;
        if (xb && live_) xpos_ = std::fmod(xpos_ + hop_ * xb->sr / patch->root()->sampleRate.load(), (double)xb->frames);
        if (live_) return true;
        sync_ = (float)std::clamp(pos_ / sb->frames, 0.0, 1.0);
        pos_ += speed * hop_ * ratio;
        if (loop_.load()) {
            pos_ = std::fmod(pos_, (double)sb->frames);
            if (pos_ < 0) pos_ += sb->frames;
        } else if (pos_ >= sb->frames || pos_ < -wn_) {
            playing_ = false;
        }
        return true;
    }
    float sample(const SampleBuffer& sb, long f, int c) const {
        if (loop_.load() && sb.frames > 0) f = ((f % sb.frames) + sb.frames) % sb.frames;
        return sb.at(f, c % sb.channels);
    }

    BufferRef ref_;                 // the buffer~ played (none: live)
    bool live_;
    int ch_, wn_, hop_;
    std::vector<float> ring_, ring2_;
    BufferRef xref_;                // the second sound of the cross-synthesis
    double xpos_ = 0.0;
    std::atomic<int> crossMode_{0};
    std::vector<std::unique_ptr<Chan>> chans_;
    double pos_ = 0.0;
    bool playing_ = false;
    int ready_ = 0, readPos_ = 0, ringPos_ = 0;
    float sync_ = 0.f;
    enum : uint8_t { kStart = kEvUser, kStop };
    std::atomic<float> threshold_{0.f};
    std::atomic<bool> loop_{false};
    std::atomic<int> cepstrum_{0}, phaseMode_{0};
    // the frames' worker: the audio thread hands it a frame (kBusy) and takes the result (kDone)
    Job job_;
    bool reset_ = false;
    unsigned gen_ = 0;
    long late_ = 0;
    std::atomic<int> state_{kIdle};
    std::atomic<bool> async_{false}, quit_{false};
    std::mutex wakeMutex_;
    std::condition_variable wake_;
    std::thread worker_;
};

// ---------------------------------------------------------------- conv~
// conv~ buffer [channels]: convolves each inlet with the impulse response in
// a buffer~ (channel c with the response's channel c, or its last one), with
// no latency. A channel whose inlet is not connected takes the previous
// channel's input, so one source into conv~ hall 2 gives a stereo reverb.
// The response is normalised to unit energy unless `normalize 0`; `gain x`
// scales it. `set name` uses another buffer~ (and reloads it).
class ConvBox : public Box {
public:
    explicit ConvBox(const Message& a) : ref_(argstr(a, 0)) {
        ch_ = std::clamp((int)argf(a, 1, 1.f), 1, 8);
        setPorts(std::vector<Port>((size_t)ch_, P::Signal), std::vector<Port>((size_t)ch_, P::Signal));
    }
    // at compile: the buffer~ looked up, the response prepared if it changed (on the GUI thread,
    // while the audio thread plays the one before) and published
    void resolve() override {
        ref_.resolve(patch);
        reload();
    }
    // a buffer~ changed, or a message: prepared aside and published (nothing compiles)
    void buffersChanged() override { reload(); }
    void message(int in, const Message& m) override {
        if (m[0].is("set") && m.size() > 1) ref_.set(patch, m[1].str());
        else if (m[0].is("gain") && m.size() > 1) gain_ = argf(m, 1, 1.f);
        else if (m[0].is("normalize")) normalize_ = argf(m, 1, 1.f) != 0.f;
        else if (m[0].isFloat()) { setScalar(in, m[0].f); return; }
        else return;
        reload();
    }
    void dsp(int n) override {
        std::vector<musil::dsp::PartConv<float>>* convs = convs_.get();
        const float* x = nullptr;
        for (int c = 0; c < ch_; ++c) {
            if (c == 0 || connected(c)) x = sigIn(c, n);   // unconnected: the channel before
            if (convs && (*convs)[(size_t)c].ready()) (*convs)[(size_t)c].process(x, sigOut(c), n);
            else dsp::fill(sigOut(c), 0.f, n);
        }
    }

private:
    void reload() {
        auto next = std::make_unique<std::vector<musil::dsp::PartConv<float>>>();
        if (prepare(*next)) convs_.set(patch, std::move(next));   // the old ones retired
    }
    // the responses of the buffer~ into next; false if nothing changed since the last time
    bool prepare(std::vector<musil::dsp::PartConv<float>>& next) {
        BufferBox* b = ref_.box();
        const SampleBuffer* sb = b ? &b->buf : nullptr;
        Key k{b, sb ? sb->data.data() : nullptr, sb ? sb->frames : 0, sb ? sb->channels : 0, b ? b->version : 0,
              gain_, normalize_};
        if (k == key_) return false;     // unchanged: keep the prepared spectra
        key_ = k;
        next.resize((size_t)ch_);
        for (int c = 0; c < ch_; ++c) {
            if (!sb || sb->frames <= 0) { next[(size_t)c].setIR(nullptr, 0, 1, 1.f); continue; }
            int irc = std::min(c, sb->channels - 1);
            double e = 0;
            for (long f = 0; f < sb->frames; ++f) e += (double)sb->at(f, irc) * sb->at(f, irc);
            float g = gain_ * (normalize_ && e > 0 ? (float)(1.0 / std::sqrt(e)) : 1.f);
            next[(size_t)c].setIR(sb->data.data() + irc, sb->frames, sb->channels, g);
        }
        return true;
    }
    struct Key {
        const void* box = nullptr;
        const float* data = nullptr;
        long frames = -1;
        int channels = 0, version = -1;
        float gain = 0.f;
        bool normalize = false;
        bool operator==(const Key& o) const {
            return box == o.box && data == o.data && frames == o.frames && channels == o.channels &&
                   version == o.version && gain == o.gain && normalize == o.normalize;
        }
    };
    BufferRef ref_;
    int ch_;
    float gain_ = 1.f;
    bool normalize_ = true;
    Key key_;
    Published<std::vector<musil::dsp::PartConv<float>>> convs_;
};

// ---------------------------------------------------------------- condec
// condec: convolution and deconvolution of whole buffers (condec), on
// demand. `conv x h out [scale] [mix]` writes x convolved with h into the
// buffer~ out (adding x * mix); `deconv y h out [threshold] [scale]` writes y
// deconvolved by h: the spectrum of y divided by that of h, where |H| is
// above threshold (0.001 by default) times its maximum, else 0. Channels
// pair up as in conv~. The outlet bangs when the result is ready.
class CondecBox : public Box {
public:
    CondecBox() { setPorts({P::Control}, {P::Control}); }
    void message(int, const Message& m) override {
        bool conv = m[0].is("conv"), deconv = m[0].is("deconv");
        if ((!conv && !deconv) || m.size() < 4) {
            if (conv || deconv) patch->print("condec: " + m[0].str() + " needs three buffer~ names");
            return;
        }
        BufferBox *xb = findBuffer(patch, m[1].str()), *hb = findBuffer(patch, m[2].str()),
                  *ob = findBuffer(patch, m[3].str());
        for (auto [b, i] : {std::pair<BufferBox*, int>{xb, 1}, {hb, 2}, {ob, 3}})
            if (!b) { patch->print("condec: no buffer~ " + m[(size_t)i].str()); return; }
        const SampleBuffer &x = xb->buf, &h = hb->buf;
        if (x.frames <= 0 || h.frames <= 0) { patch->print("condec: an empty buffer~"); return; }
        float p1 = argf(m, 4, conv ? 1.f : 0.001f), p2 = argf(m, 5, conv ? 0.f : 1.f);
        float scale = conv ? p1 : p2, mix = conv ? p2 : 0.f, threshold = conv ? 0.f : p1;
        long outLen = conv ? x.frames + h.frames - 1 : x.frames;
        int ch = std::max(x.channels, h.channels);
        SampleBuffer out;
        out.channels = ch;
        out.sr = x.sr;
        out.frames = outLen;
        out.data.assign((size_t)outLen * ch, 0.f);
        std::vector<double> xs((size_t)x.frames), hs((size_t)h.frames);
        for (int c = 0; c < ch; ++c) {        // Musil's convolution and deconvolution (signals/convolution.h)
            int xc = std::min(c, x.channels - 1), hc = std::min(c, h.channels - 1);
            for (long f = 0; f < x.frames; ++f) xs[(size_t)f] = x.at(f, xc);
            for (long f = 0; f < h.frames; ++f) hs[(size_t)f] = h.at(f, hc);
            std::vector<double> y = conv ? musil::dsp::convolve(xs, hs) : musil::dsp::deconvolve(xs, hs, threshold);
            for (long f = 0; f < outLen; ++f) {
                double v = y[(size_t)f] * scale;
                if (conv && f < x.frames) v += xs[(size_t)f] * mix;
                out.data[(size_t)f * ch + c] = std::isfinite(v) ? (float)v : 0.f;
            }
        }
        ob->replace(std::move(out));   // swapped in under the lock; conv~ and the others see it
        outletBang(0);
    }
};

// ---------------------------------------------------------------- function
// function [domain] [lo] [hi] [x y ...]: a breakpoint editor. In locked mode,
// click to add a point, drag to move it, Shift-click to delete it; the first
// and last points stay at the ends. bang outputs, from the right outlet, the
// first value and then the rest as "value time ..." pairs: exactly what line~
// (or line) needs to play the curve over `domain` ms. A number x (ms) outputs
// the curve's value there from the left outlet. `domain ms` rescales the
// curve in time, `range lo hi` sets the vertical range, `clear` resets it,
// `set x y x y ...` replaces the points. The points are saved with the patch.
class FunctionBox : public Box {
public:
    explicit FunctionBox(const Message& a) {
        kind = BoxKind::UI;
        setPorts({P::Control}, {P::Control, P::Control});
        domain_ = std::max(1.f, argf(a, 0, 1000.f));
        lo_ = argf(a, 1, 0.f);
        hi_ = argf(a, 2, 1.f);
        if (hi_ == lo_) hi_ = lo_ + 1.f;
        for (size_t k = 3; k + 1 < a.size(); k += 2)
            if (a[k].isFloat() && a[k + 1].isFloat()) pts_.push_back({a[k].f, a[k + 1].f});
        tidy();
    }
    void uiSize(int& w, int& h) const override { w = 280; h = 110; }
    Message saveArgs() const override {
        Message m{Atom(domain_), Atom(lo_), Atom(hi_)};
        for (auto& p : pts_) { m.push_back(Atom(p.first)); m.push_back(Atom(p.second)); }
        return m;
    }
    void message(int, const Message& m) override {
        if (m[0].is("bang")) {
            outletFloat(1, pts_[0].second);
            Message seg;
            for (size_t k = 1; k < pts_.size(); ++k) {
                seg.push_back(Atom(pts_[k].second));
                seg.push_back(Atom(pts_[k].first - pts_[k - 1].first));
            }
            if (!seg.empty()) outlet(1, seg);
        } else if (m[0].isFloat()) {
            outletFloat(0, valueAt(m[0].f));
        } else if (m[0].is("domain") && m.size() > 1) {
            float d = std::max(1.f, argf(m, 1, domain_));
            for (auto& p : pts_) p.first *= d / domain_;
            domain_ = d;
            tidy();
        } else if (m[0].is("range") && m.size() > 2) {
            lo_ = argf(m, 1, lo_);
            hi_ = argf(m, 2, hi_);
            if (hi_ == lo_) hi_ = lo_ + 1.f;
            tidy();
        } else if (m[0].is("clear")) {
            pts_.clear();
            tidy();
        } else if (m[0].is("set")) {
            pts_.clear();
            for (size_t k = 1; k + 1 < m.size(); k += 2)
                if (m[k].isFloat() && m[k + 1].isFloat()) pts_.push_back({m[k].f, m[k + 1].f});
            tidy();
        }
        patch->redraw();
    }
    void uiMouse(int e, int mx, int my) override {
        float x = std::clamp((mx - kPad) / (float)(w - 2 * kPad), 0.f, 1.f) * domain_;
        float y = lo_ + std::clamp(1.f - (my - kPad) / (float)(h - 2 * kPad), 0.f, 1.f) * (hi_ - lo_);
        if (e == FL_PUSH) {
            drag_ = -1;
            for (size_t k = 0; k < pts_.size(); ++k)
                if (std::abs(px(pts_[k].first) - mx) <= 5 && std::abs(py(pts_[k].second) - my) <= 5) drag_ = (int)k;
            if (drag_ >= 0 && Fl::event_state(FL_SHIFT)) {          // delete (not the ends)
                if (drag_ > 0 && drag_ < (int)pts_.size() - 1) pts_.erase(pts_.begin() + drag_);
                drag_ = -1;
            } else if (drag_ < 0) {                                 // new point
                auto it = std::upper_bound(pts_.begin(), pts_.end(), x,
                                           [](float v, const std::pair<float, float>& p) { return v < p.first; });
                drag_ = (int)(it - pts_.begin());
                if (drag_ == 0) drag_ = 1;
                if (drag_ >= (int)pts_.size()) drag_ = (int)pts_.size() - 1;
                pts_.insert(pts_.begin() + drag_, {x, y});
            }
        }
        if ((e == FL_PUSH || e == FL_DRAG) && drag_ >= 0) {
            auto& p = pts_[(size_t)drag_];
            p.second = y;
            if (drag_ > 0 && drag_ < (int)pts_.size() - 1)
                p.first = std::clamp(x, pts_[(size_t)drag_ - 1].first, pts_[(size_t)drag_ + 1].first);
        }
        if (e == FL_RELEASE) drag_ = -1;
        patch->redraw();
    }
    void drawUI(int X, int Y) override {
        fl_color(fl_rgb_color(22, 24, 28));
        zd::rectf(X + 3, Y + 3, w - 6, h - 6);
        fl_color(fl_rgb_color(42, 46, 54));
        for (int k = 1; k < 4; ++k) zd::xyline(X + 3, Y + kPad + k * (h - 2 * kPad) / 4, X + w - 4);
        fl_color(kAccent);
        zd::line_style(FL_SOLID, 2);
        zd::begin_line();
        for (auto& p : pts_) zd::vertex(X + px(p.first), Y + py(p.second));
        zd::end_line();
        zd::line_style(0);
        for (auto& p : pts_) {
            fl_color(kText);
            zd::rectf(X + px(p.first) - 3, Y + py(p.second) - 3, 7, 7);
        }
        zd::font(kFont, 9);
        fl_color(kDim);
        char label[64];
        std::snprintf(label, sizeof label, "%g..%g  %g ms", lo_, hi_, domain_);
        zd::draw(label, X + 6, Y + 13);
    }

private:
    static constexpr int kPad = 8;
    int px(float x) const { return kPad + (int)std::lround(x / domain_ * (w - 2 * kPad)); }
    int py(float y) const { return kPad + (int)std::lround((1.f - (y - lo_) / (hi_ - lo_)) * (h - 2 * kPad)); }
    float valueAt(float x) const {
        if (x <= pts_.front().first) return pts_.front().second;
        for (size_t k = 1; k < pts_.size(); ++k)
            if (x <= pts_[k].first) {
                float x0 = pts_[k - 1].first, x1 = pts_[k].first;
                float t = x1 > x0 ? (x - x0) / (x1 - x0) : 1.f;
                return pts_[k - 1].second + t * (pts_[k].second - pts_[k - 1].second);
            }
        return pts_.back().second;
    }
    // sorted, inside the domain and range, with points at both ends
    void tidy() {
        for (auto& p : pts_) {
            p.first = std::clamp(p.first, 0.f, domain_);
            p.second = std::clamp(p.second, std::min(lo_, hi_), std::max(lo_, hi_));
        }
        std::stable_sort(pts_.begin(), pts_.end(), [](auto& a, auto& b) { return a.first < b.first; });
        if (pts_.empty() || pts_.front().first > 0.f) pts_.insert(pts_.begin(), {0.f, pts_.empty() ? lo_ : pts_.front().second});
        if (pts_.size() < 2 || pts_.back().first < domain_) pts_.push_back({domain_, pts_.back().second});
        pts_.front().first = 0.f;
        pts_.back().first = domain_;
    }
    float domain_, lo_, hi_;
    std::vector<std::pair<float, float>> pts_;
    int drag_ = -1;
};

// ---------------------------------------------------------------- nebula~
// nebula~ [proto1] [proto2]: sound_math's nebula, advanced granular
// synthesis; the engine of Musil's nebula (musil/src/signals/nebula.h). Grains are windowed snippets of two prototypes, each a waveform
// (sine, triangle, square, noise) or a buffer~, mixed by the morph amount.
// A grain starts every 1 / density seconds and lasts `length` ms; its
// frequency is in Hz for a waveform and a speed ratio for a buffer~ (1 = as
// recorded: the original's relative pitch, the frequency times the sound's
// natural frequency), and it can glide; it is placed in the stereo field at
// random. `pitch1 hz` (or `pitch2`) gives the pitch of a sound prototype:
// its frequency is then in Hz too (the speed is freq / pitch), so a waveform
// and a sound can be morphed at the same pitch; `pitch1 0` returns to ratios.
// The values after a parameter are randomness, as a ratio of it: `density
// 40 .5` gives 20 to 60 grains a second.
//   inlets: 1/0 or start/stop, density, length (ms), frequency, morph 0..1
//   messages: density d [r], length ms [r], freq f [r], morph m, amp a,
//   gliss g (each grain glides to f within +-g f), jitter j [r] (pan, 0 right
//   .. 1 left, randomised by r), speed s (how fast the read position moves
//   through a buffer~), grainlets n (each grain lasts n periods of its
//   frequency; 0 = off), window hann|hamming|blackman, poly n, proto1 x,
//   proto2 x, pitch1 hz, pitch2 hz, seed n.
class NebulaBox : public Box {
public:
    explicit NebulaBox(const Message& a) {
        setPorts({P::Control, P::Control, P::Control, P::Control, P::Control}, {P::Signal, P::Signal});
        names_[0] = a.size() > 0 ? a[0].str() : "sine";
        names_[1] = a.size() > 1 ? a[1].str() : names_[0];
        for (int k = 0; k < 2; ++k) neb_.setWave(k, names_[k]);
    }
    void resolve() override {
        for (int k = 0; k < 2; ++k) buf_[k].set(patch, Neb::isWave(names_[k]) ? "" : names_[k]);
    }
    void message(int in, const Message& m) override {
        auto num = [&](size_t k, float def) { return argf(m, k, def); };
        if (in > 0) {
            if (!m[0].isFloat()) return;
            if (in == 1) density_.store(std::max(.01f, m[0].f));
            else if (in == 2) length_.store(std::max(1.f, m[0].f));
            else if (in == 3) freq_.store(m[0].f);
            else morph_.store(std::clamp(m[0].f, 0.f, 1.f));
            return;
        }
        const Atom& h = m[0];
        if (h.isFloat()) on_.store(h.f != 0.f);
        else if (h.is("start") || h.is("bang")) on_.store(true);
        else if (h.is("stop")) on_.store(false);
        else if (h.is("density")) { density_.store(std::max(.01f, num(1, 10))); densityRnd_.store(std::clamp(num(2, 0), 0.f, .99f)); }
        else if (h.is("length")) { length_.store(std::max(1.f, num(1, 100))); lengthRnd_.store(std::clamp(num(2, 0), 0.f, 1.f)); }
        else if (h.is("freq")) { freq_.store(num(1, 220)); freqRnd_.store(std::clamp(num(2, 0), 0.f, 1.f)); }
        else if (h.is("morph")) morph_.store(std::clamp(num(1, 0), 0.f, 1.f));
        else if (h.is("amp")) amp_.store(std::max(0.f, num(1, .25f)));
        else if (h.is("gliss")) gliss_.store(std::clamp(num(1, 0), 0.f, 1.f));
        else if (h.is("jitter")) { jitter_.store(std::clamp(num(1, .5f), 0.f, 1.f)); jitterRnd_.store(std::clamp(num(2, 1), 0.f, 1.f)); }
        else if (h.is("speed")) speed_.store(num(1, 1));
        else if (h.is("grainlets")) grainlets_.store(std::max(0.f, num(1, 0)));
        else if (h.is("poly")) poly_.store(std::clamp((int)num(1, 256), 1, Neb::kMaxPoly));
        else if (h.is("seed")) post(kSeed, num(1, 1));
        else if (h.is("window") && m.size() > 1) {
            std::lock_guard<std::mutex> lk(patch->mutex());
            neb_.setWindow(m[1].str());
        } else if ((h.is("pitch1") || h.is("pitch2")) && m.size() > 1) {
            pitch_[h.is("pitch1") ? 0 : 1].store(std::max(0.f, num(1, 0)));
        } else if ((h.is("proto1") || h.is("proto2")) && m.size() > 1) {
            int k = h.is("proto1") ? 0 : 1;
            std::string name = m[1].str();
            bool wave = neb_.prepareWave(k, name);       // the tables are built outside the lock
            std::lock_guard<std::mutex> lk(patch->mutex());
            names_[k] = name;
            if (wave) neb_.commitWave(k);                // only swapped under it
            else neb_.setSound(k, nullptr, 0, 1, 44100.0);
            buf_[k].set(patch, wave ? "" : name);
        }
    }
    void dsp(int n) override {
        for (int k = 0; k < events(); ++k)
            if (event(k).kind == kSeed) neb_.seed((unsigned)event(k).a);
        if (!audioBuffersAvailable()) {        // the GUI holds the lock: it is changing neb_ (proto, window)
            dsp::fill(sigOut(0), 0.f, n);
            dsp::fill(sigOut(1), 0.f, n);
            return;
        }
        auto& p = neb_.p;
        p.on = on_.load(std::memory_order_relaxed);
        p.density = density_.load(); p.densityRnd = densityRnd_.load();
        p.length = length_.load(); p.lengthRnd = lengthRnd_.load();
        p.freq = freq_.load(); p.freqRnd = freqRnd_.load();
        p.morph = morph_.load(); p.amp = amp_.load(); p.gliss = gliss_.load();
        p.jitter = jitter_.load(); p.jitterRnd = jitterRnd_.load();
        p.speed = speed_.load(); p.grainlets = grainlets_.load(); p.poly = poly_.load();
        for (int k = 0; k < 2; ++k) {
            p.pitch[k] = pitch_[k].load(std::memory_order_relaxed);
            if (Neb::isWave(names_[k])) continue;
            const SampleBuffer* b = buf_[k].get();        // the buffer as it is now
            if (b && b->frames > 0) neb_.setSound(k, b->data.data(), b->frames, b->channels, b->sr);
            else neb_.setSound(k, nullptr, 0, 1, 44100.0);
        }
        neb_.process(sigOut(0), sigOut(1), n, (double)patch->root()->sampleRate.load());
    }

private:
    using Neb = musil::dsp::Nebula<float>;     // Musil's nebula (musil/src/signals/nebula.h)
    Neb neb_;
    std::string names_[2];
    BufferRef buf_[2];
    std::atomic<bool> on_{false};
    std::atomic<float> density_{10.f}, densityRnd_{0.f}, length_{100.f}, lengthRnd_{0.f}, freq_{220.f},
        freqRnd_{0.f}, morph_{0.f}, amp_{.25f}, gliss_{0.f}, jitter_{.5f}, jitterRnd_{1.f}, speed_{1.f},
        grainlets_{0.f};
    std::atomic<float> pitch_[2] = {0.f, 0.f};
    std::atomic<int> poly_{256};
    enum : uint8_t { kSeed = kEvUser };
};

// ---------------------------------------------------------------- harmonize~
// harmonize~ [voices] [latency-ms] [separate]: reVoice's vocal harmonizer, the
// engine of Musil's harmonize (musil/src/signals/psola.h): up to 8 copies of
// the voice in its left inlet at other pitches, made of grains of its own
// periods (PSOLA), so the formants stay. The voices follow YIN's f0 of the
// input; where it is unvoiced they fade out. Modes: intervals (semitones per
// voice, the default 4 7 12 -12), scale (the sung note snapped to key and
// scale, each voice that many scale steps from it), midi-chord (each held
// note a voice, velocity its level), midi-root (the last held note the
// fundamental, the voices their intervals above it). Notes come as `note p v`,
// or to the two right inlets as notein and kslider send them (velocity, then
// pitch).
// The outlets are left and right (with `separate`, one per voice, not
// panned, no dry) and the f0 detected (Hz, 0 unvoiced), every 20 ms when it
// changes. Values reach the audio thread through atomics, notes through the
// box's event queue; a new range or latency builds a new engine aside and
// swaps it in.
class HarmonizeBox : public Box {
public:
    using Ps = musil::dsp::Psola<float>;
    static constexpr int kMax = Ps::kMaxVoices;
    explicit HarmonizeBox(const Message& a) {
        int k = 0;
        for (auto& x : a) {
            if (x.isSymbol() && x.s == "separate") separate_ = true;
            else if (x.isFloat()) {
                if (k == 0) nvoices_ = std::clamp((int)x.f, 1, kMax);
                else if (k == 1) latencyMs_ = std::clamp(x.f, 0.f, 500.f);
                ++k;
            }
        }
        active_.store(nvoices_);
        static const float iv[kMax] = {4, 7, 12, -12, -5, 3, 9, -8};
        for (int v = 0; v < kMax; ++v) {
            interval_[v].store(iv[v]);
            pan_[v].store(nvoices_ > 1 ? -0.7f + 1.4f * (float)(v % nvoices_) / (float)(nvoices_ - 1) : 0.f);
        }
        std::vector<Port> outs((size_t)(separate_ ? nvoices_ : 2), P::Signal);
        outs.push_back(P::Control);
        setPorts({P::Signal, P::Control, P::Control, P::Control}, outs);
    }
    void attached() override { tick(); }
    void resolve() override {
        double sr = patch->root()->sampleRate.load();
        if (!(sr > 0)) sr = 44100;
        Ps* cur = eng_.get();
        if (!cur || cur->sampleRate() != sr) rebuild(sr);
    }
    void message(int in, const Message& m) override {
        if (m.empty()) return;
        const Atom& h = m[0];
        if (in == 3) { if (h.isFloat()) vel_ = h.f; return; }      // the velocity of the next pitch
        if (in == 2) {                                            // a pitch, or pitch velocity
            if (h.isFloat()) note((int)h.f, m.size() >= 2 ? argf(m, 1, 0.f) : vel_);
            return;
        }
        if (in == 1 || (h.isFloat() && in == 0)) { intervals(m, 0); return; }
        if (h.is("intervals") || h.is("list")) intervals(m, 1);
        else if (h.is("note") && m.size() >= 3) note((int)argf(m, 1, 60.f), argf(m, 2, 0.f));
        else if (h.is("flush") || h.is("allnotesoff")) { held_.fill(0.f); post(kAllOff); }
        else if (h.is("mode") && m.size() > 1) {
            musil::dsp::HarmonyMode md;
            if (musil::dsp::harmony_mode(m[1].str(), md)) mode_.store((int)md);
            else patch->print("harmonize~: mode is intervals, scale, midi-chord or midi-root");
        } else if (h.is("key") && m.size() > 1) {
            int k = m[1].isFloat() ? (((int)std::lround(m[1].f) % 12) + 12) % 12 : musil::music::key_class(m[1].str());
            if (k < 0) { patch->print("harmonize~: a key is 0..11 or a name (C, F#, Bb ...)"); return; }
            key_ = k;
            scaleMask_.store(musil::music::scale_mask(steps_.data(), nsteps_, key_));
        } else if (h.is("scale") && m.size() > 1) {
            if (m[1].isFloat()) {
                nsteps_ = 0;
                for (size_t k = 1; k < m.size() && nsteps_ < 12; ++k) steps_[(size_t)nsteps_++] = (((int)std::lround(argf(m, k, 0.f)) % 12) + 12) % 12;
            } else {
                const musil::music::ScaleDef* d = musil::music::scale_named(m[1].str());
                if (!d) { patch->print("harmonize~: unknown scale " + m[1].str()); return; }
                nsteps_ = d->n;
                for (int k = 0; k < d->n; ++k) steps_[(size_t)k] = d->steps[k];
            }
            scaleMask_.store(musil::music::scale_mask(steps_.data(), nsteps_, key_));
        } else if (h.is("voice") && m.size() > 2) {
            int v = (int)argf(m, 1, 1.f) - 1;
            if (v < 0 || v >= kMax) return;
            Message rest;
            for (size_t k = 2; k < m.size(); ++k) rest.push_back(m[k]);
            voiceParam(v, v + 1, rest);
        } else if (h.is("level") || h.is("pan") || h.is("formant") || h.is("vibrato") || h.is("interval")) voiceParam(0, kMax, m);
        else if (h.is("mix")) { dry_.store(std::max(0.f, argf(m, 1, 0.f))); wet_.store(std::max(0.f, argf(m, 2, 1.f))); }
        else if (h.is("transpose")) transpose_.store(argf(m, 1, 0.f));
        else if (h.is("humanize")) { humCents_.store(std::max(0.f, argf(m, 1, 0.f))); humMs_.store(std::max(0.f, argf(m, 2, 0.f))); }
        else if (h.is("cycles")) cycles_.store(m.size() > 1 && m[1].isFloat() ? std::clamp(m[1].f, 0.5f, 4.f) : 0.f);
        else if (h.is("threshold")) threshold_.store(std::clamp(argf(m, 1, .15f), .001f, 1.f));
        else if (h.is("gate")) gate_.store(std::pow(10.f, argf(m, 1, -50.f) / 20.f));
        else if (h.is("window") && m.size() > 1) {
            std::string w = m[1].str();
            int k = w == "hamming" ? 1 : w == "blackman" ? 2 : w == "hann" ? 0 : -1;
            if (k < 0) patch->print("harmonize~: window is hann, hamming or blackman");
            else window_.store(k);
        } else if (h.is("marks") && m.size() > 1) marksPeriod_.store(m[1].is("period"));
        else if (h.is("unvoiced") && m.size() > 1) unvoicedDry_.store(m[1].is("dry"));
        else if (h.is("seed")) post(kSeed, argf(m, 1, 1.f));
        else if (h.is("range") && m.size() > 2) {
            float lo = argf(m, 1, 70.f), hi = argf(m, 2, 1000.f);
            if (!(lo >= 20.f) || !(hi > lo * 1.5f)) { patch->print("harmonize~: range lo hi, 20 <= lo, hi > 1.5 lo"); return; }
            fmin_ = lo; fmax_ = hi;
            rebuild(eng_.get() ? eng_.get()->sampleRate() : 44100.0);
        } else if (h.is("latency")) {
            latencyMs_ = std::clamp(argf(m, 1, 0.f), 0.f, 500.f);
            rebuild(eng_.get() ? eng_.get()->sampleRate() : 44100.0);
        }
    }
    void dsp(int n) override {
        Ps* ps = eng_.get();
        const int nout = separate_ ? nvoices_ : 2;
        if (!ps) {
            for (int k = 0; k < nout; ++k) dsp::fill(sigOut(k), 0.f, n);
            return;
        }
        auto& p = ps->p;
        p.mode = (musil::dsp::HarmonyMode)mode_.load(std::memory_order_relaxed);
        p.voices = std::clamp(active_.load(std::memory_order_relaxed), 1, nvoices_);
        for (int v = 0; v < kMax; ++v) {
            p.v[v].interval = interval_[v].load(std::memory_order_relaxed);
            p.v[v].level = level_[v].load(std::memory_order_relaxed);
            p.v[v].pan = pan_[v].load(std::memory_order_relaxed);
            p.v[v].formant = formant_[v].load(std::memory_order_relaxed);
            p.v[v].vibDepth = vibDepth_[v].load(std::memory_order_relaxed);
            p.v[v].vibRate = vibRate_[v].load(std::memory_order_relaxed);
            p.v[v].vibRandom = vibRandom_[v].load(std::memory_order_relaxed);
        }
        p.transpose = transpose_.load(std::memory_order_relaxed);
        p.scaleMask = scaleMask_.load(std::memory_order_relaxed);
        p.dry = dry_.load(std::memory_order_relaxed);
        p.wet = wet_.load(std::memory_order_relaxed);
        p.humanizeCents = humCents_.load(std::memory_order_relaxed);
        p.humanizeMs = humMs_.load(std::memory_order_relaxed);
        p.cycles = cycles_.load(std::memory_order_relaxed);
        p.unvoicedDry = unvoicedDry_.load(std::memory_order_relaxed);
        p.marksPeriod = marksPeriod_.load(std::memory_order_relaxed);
        ps->setThreshold(threshold_.load(std::memory_order_relaxed));
        ps->setGate(gate_.load(std::memory_order_relaxed));
        int w = window_.load(std::memory_order_relaxed);
        if (w != ps->windowType()) ps->setWindowType(w);           // the table is rewritten in place
        const float* x = sigIn(0, n);
        float* outs[kMax] = {};
        auto run = [&](int from, int to) {
            if (to <= from) return;
            if (separate_) {
                for (int k = 0; k < nvoices_; ++k) outs[k] = sigOut(k) + from;
                ps->process(x + from, nullptr, nullptr, to - from, outs);
            } else ps->process(x + from, sigOut(0) + from, sigOut(1) + from, to - from);
        };
        int at = 0;
        for (int k = 0; k < events(); ++k) {                     // notes at their sample
            const Event& e = event(k);
            int off = std::clamp(eventOffset(k), at, n);
            run(at, off);
            at = off;
            if (e.kind == kNote) ps->noteOn((int)e.a, e.b);
            else if (e.kind == kAllOff) ps->allNotesOff();
            else if (e.kind == kSeed) ps->seed((uint32_t)e.a);
        }
        run(at, n);
        f0_.store((float)ps->f0(), std::memory_order_relaxed);
    }

private:
    enum : uint8_t { kNote = kEvUser, kAllOff, kSeed };
    // a list of intervals (or scale steps): the voices that sound, up to the box's
    void intervals(const Message& m, size_t from) {
        int k = 0;
        for (size_t i = from; i < m.size() && k < nvoices_; ++i)
            if (m[i].isFloat()) interval_[k++].store(m[i].f);
        if (k > 0) active_.store(k);
    }
    // level x, pan x, formant x, vibrato depth rate [random], interval x: voices from .. to - 1
    void voiceParam(int from, int to, const Message& m) {
        if (m.empty()) return;
        const Atom& h = m[0];
        for (int v = from; v < to; ++v) {
            if (h.is("level")) level_[v].store(std::max(0.f, argf(m, 1, 1.f)));
            else if (h.is("pan")) pan_[v].store(std::clamp(argf(m, 1, 0.f), -1.f, 1.f));
            else if (h.is("formant")) formant_[v].store(std::clamp(argf(m, 1, 1.f), .25f, 2.f));
            else if (h.is("interval")) interval_[v].store(argf(m, 1, 0.f));
            else if (h.is("vibrato")) {
                vibDepth_[v].store(std::max(0.f, argf(m, 1, 0.f)));
                vibRate_[v].store(std::max(0.f, argf(m, 2, 5.f)));
                vibRandom_[v].store(std::clamp(argf(m, 3, 0.f), 0.f, 1.f));
            }
        }
    }
    void note(int pitch, float vel) {
        if (pitch < 0 || pitch > 127) return;
        held_[(size_t)pitch] = std::max(0.f, vel);
        post(kNote, (float)pitch, std::max(0.f, vel));
    }
    // a new engine for the rate, range and latency, built here and swapped in; the notes held are given again
    void rebuild(double sr) {
        auto e = std::make_unique<Ps>();
        Ps::Setup s;
        s.sr = sr; s.fmin = fmin_; s.fmax = fmax_; s.latencyMs = latencyMs_;
        s.threshold = threshold_.load(); s.gate = gate_.load();
        e->init(s);
        e->setWindowType(window_.load());
        eng_.set(patch, std::move(e));
        post(kAllOff);
        for (int k = 0; k < 128; ++k) if (held_[(size_t)k] > 0) post(kNote, (float)k, held_[(size_t)k]);
    }
    // the f0 out, every 20 ms, when it changed
    void tick() {
        float f = f0_.load(std::memory_order_relaxed);
        if (patch->root()->dspRunning && std::fabs(f - sent_) > 0.01f) {
            sent_ = f;
            outletFloat(numOuts() - 1, f);
        }
        patch->schedule(this, 0.02, [this] { tick(); });
    }

    Published<Ps> eng_;
    int nvoices_ = 4;
    bool separate_ = false;
    float latencyMs_ = 0, fmin_ = 70, fmax_ = 1000, sent_ = -1, vel_ = 100;
    int key_ = 0, nsteps_ = 7;
    std::array<int, 12> steps_ = {0, 2, 4, 5, 7, 9, 11};
    std::array<float, 128> held_{};
    std::atomic<int> active_{4}, mode_{0}, scaleMask_{0xAB5}, window_{0};
    std::atomic<float> interval_[kMax], level_[kMax] = {1, 1, 1, 1, 1, 1, 1, 1}, pan_[kMax], formant_[kMax] = {1, 1, 1, 1, 1, 1, 1, 1},
        vibDepth_[kMax] = {}, vibRate_[kMax] = {5, 5, 5, 5, 5, 5, 5, 5}, vibRandom_[kMax] = {};
    std::atomic<float> transpose_{0}, dry_{0}, wet_{1}, humCents_{0}, humMs_{0}, cycles_{0}, threshold_{.15f}, gate_{.003f}, f0_{0};
    std::atomic<bool> unvoicedDry_{false}, marksPeriod_{false};
};

}  // namespace

void registerSoundObjects(ClassTable& t) {
    t.insert(t.end(), {
        {"sparkle~", "Spectral", "phase vocoder: [buffer] [channels] [window] [overlap]", with<SparkleBox>()},
        {"conv~", "Filters", "zero-latency convolution with a buffer~: buffer [channels]", with<ConvBox>()},
        {"condec", "Samples", "convolve / deconvolve buffers: conv|deconv x h out", plain<CondecBox>()},
        {"function", "Interface", "breakpoint editor: [domain ms] [lo] [hi]", with<FunctionBox>()},
        {"nebula~", "Oscillators", "granular synthesis (sound_math's nebula): [proto1] [proto2]", with<NebulaBox>()},
        {"harmonize~", "Filters", "vocal harmonizer (reVoice): [voices] [latency-ms] [separate]", with<HarmonizeBox>()},
    });
}

}  // namespace moritz
