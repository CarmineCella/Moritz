// Copyright (c) 2024-2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_sound.cpp — sound processing from the author's tools: sparkle~
// (sparkle's phase vocoder), conv~ (pconv/condec convolution), condec
// (convolution and deconvolution of buffers), descriptors~ (low-level
// descriptors and f0) and function (a breakpoint editor)

#include "objects.h"
#include "signals/fft.h"   // Musil's FFT
#include "zoom.h"
#include "signals/convolution.h"   // Musil's convolution engines
#include "signals/vocoder.h"   // Musil's phase vocoder engine, as its pvoc
#include "signals/nebula.h"    // Musil's granular engine, as its nebula

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <atomic>
#include <mutex>
#include <cstdio>

namespace moritz {
namespace {

int nextPow2(long n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

// ---------------------------------------------------------------- sparkle~
// sparkle~ [buffer] [channels] [window] [overlap]: sparkle's phase vocoder,
// the engine of Musil's pvoc (musil/src/signals/vocoder.h).
// With a buffer~ name it plays the buffer: the left inlet is the speed (1 =
// as recorded, 0.25 = four times slower, 0 = frozen, negative = backwards),
// `start [ms]` or bang plays, `stop` stops, `loop 1` loops. Without a name it
// transforms the signal in its left inlet (latency: one window).
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
        setScalar(3, .5f);                  // cross amount
        ready_ = 0;
    }
    void resolve() override {
        if (!live_) ref_.resolve(patch);
        xref_.resolve(patch);
    }
    void message(int in, const Message& m) override {
        if (in > 0) { if (m[0].isFloat()) setScalar(in, m[0].f); return; }
        if (m[0].isFloat()) { setScalar(0, m[0].f); return; }
        if (m[0].is("bang")) startAt_.store(0.f);
        else if (m[0].is("start")) startAt_.store(std::max(0.f, argf(m, 1, 0.f)));
        else if (m[0].is("stop")) stop_.store(true);
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
        if (float s = startAt_.exchange(-1.f); s >= 0.f && sb) {
            pos_ = s / 1000.0 * sb->sr;
            playing_ = true;
            for (auto& c : chans_) { c->voc->reset(); std::fill(c->ola.begin(), c->ola.end(), 0.f); }
            ready_ = 0;
        }
        if (stop_.exchange(false)) playing_ = false;
        for (int i = 0; i < n; ++i) {
            if (live_) ring_[(size_t)ringPos_] = spd[i];        // keep the input(s)
            ring2_[(size_t)ringPos_] = other[i];
            ringPos_ = (ringPos_ + 1) % (int)ring2_.size();
            if (ready_ == 0) {
                frame(pitch[i], form[i], spd[i], amount[i]);
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
    // one analysis/synthesis frame: overlap-adds a window into ola
    void frame(float pitch, float formant, float speed, float amount) {
        const SampleBuffer* sb = live_ ? nullptr : ref_.get();
        bool active = live_ || (playing_ && sb && sb->frames > wn_);
        if (!active) { sync_ = 0.f; return; }
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
            x.voc->phaseMode = phaseMode_.load();
            x.voc->crossMode = xmode;
            x.voc->envelope = C;
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
            x.voc->process(x.now.data(), x.prev.data(), xmode ? x.oNow.data() : nullptr, xmode ? x.oPrev.data() : nullptr,
                           (double)D, (double)hop_, P, F, thr, amount, x.out.data());
            for (int k = 0; k < wn_; ++k) x.ola[(size_t)k] += x.out[(size_t)k];
        }
        if (xb && live_) xpos_ = std::fmod(xpos_ + hop_ * xb->sr / patch->root()->sampleRate.load(), (double)xb->frames);
        if (live_) return;
        sync_ = (float)std::clamp(pos_ / sb->frames, 0.0, 1.0);
        pos_ += speed * hop_ * ratio;
        if (loop_.load()) {
            pos_ = std::fmod(pos_, (double)sb->frames);
            if (pos_ < 0) pos_ += sb->frames;
        } else if (pos_ >= sb->frames || pos_ < -wn_) {
            playing_ = false;
        }
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
    std::atomic<float> startAt_{-1.f}, threshold_{0.f};
    std::atomic<bool> stop_{false}, loop_{false};
    std::atomic<int> cepstrum_{0}, phaseMode_{0};
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

// ---------------------------------------------------------------- descriptors~
// descriptors~ [size] [hop] [names...]: low-level descriptors of its signal,
// every hop samples (1024 by default) over a frame of size samples (2048),
// while audio runs; bang analyses now. Names choose the outlets and their
// order (all of them, in this order, by default):
//   rms          root mean square level
//   centroid     spectral centroid (Hz): the amplitude-weighted mean frequency
//   spread       spectral spread (Hz): standard deviation around the centroid
//   skewness     asymmetry of the spectrum around the centroid
//   kurtosis     peakedness of the spectrum (3 for a Gaussian shape)
//   flux         spectral flux: how much the spectrum grew since the last frame
//   hfc          high frequency content: energy weighted by bin number
//   zcr          zero crossings per second
//   flatness     geometric over arithmetic mean of the spectrum (0 tonal .. 1 noise)
//   periodicity  height of the autocorrelation peak (0..1): how periodic the frame is
//   f0           fundamental frequency (Hz) from the autocorrelation; 0 when unvoiced
// The formulas are those of sound_math's features.h; f0 follows its
// acfF0Estimate, with the autocorrelation computed by FFT.
class DescriptorsBox : public Box {
public:
    explicit DescriptorsBox(const Message& a) : ring_(new std::atomic<float>[kRing]) {
        std::vector<float> nums;
        for (auto& x : a) {
            if (x.isFloat()) nums.push_back(x.f);
            else if (index(x.s) >= 0) which_.push_back(index(x.s));
        }
        n_ = std::clamp(nextPow2((long)(nums.size() > 0 ? nums[0] : 2048)), 256, kRing / 2);
        hop_ = std::clamp((int)(nums.size() > 1 ? nums[1] : 1024), 32, 65536);
        if (which_.empty())
            for (int k = 0; k < kCount; ++k) which_.push_back(k);
        setPorts({P::Signal}, std::vector<Port>(which_.size(), P::Control));
        for (int i = 0; i < kRing; ++i) ring_[i].store(0.f);
        fft_.init(2 * n_);
        prevAmp_.assign((size_t)n_ / 2 + 1, 0.f);
    }
    void attached() override { tick(); }
    void message(int, const Message& m) override {
        if (m[0].is("bang")) analyse();
        else if (m[0].is("hop") && m.size() > 1) hop_ = std::clamp((int)argf(m, 1, 1024.f), 32, 65536);
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        size_t w = wpos_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) ring_[(w + i) & (kRing - 1)].store(x[i], std::memory_order_relaxed);
        wpos_.store(w + n, std::memory_order_release);
    }
    static const char* name(int k) {
        static const char* const names[kCount] = {"rms", "centroid", "spread", "skewness", "kurtosis", "flux",
                                                   "hfc", "zcr", "flatness", "periodicity", "f0"};
        return names[k];
    }

private:
    static constexpr int kRing = 16384, kCount = 11;
    static int index(const std::string& s) {
        for (int k = 0; k < kCount; ++k)
            if (s == name(k)) return k;
        return -1;
    }
    void tick() {
        if (patch->root()->dspRunning) analyse();
        double sr = patch->root()->sampleRate.load();
        patch->schedule(this, hop_ / (sr > 0 ? sr : 44100.0), [this] { tick(); });
    }
    void analyse() {
        double sr = patch->root()->sampleRate.load();
        size_t end = wpos_.load(std::memory_order_acquire);
        int N = n_, H = N / 2;
        std::vector<float> x((size_t)N);
        for (int i = 0; i < N; ++i) x[(size_t)i] = ring_[(end - N + (size_t)i) & (kRing - 1)].load(std::memory_order_relaxed);
        float v[kCount] = {};
        // temporal: level and zero crossings
        double e = 0;
        int zc = 0;
        for (int i = 0; i < N; ++i) {
            e += (double)x[(size_t)i] * x[(size_t)i];
            if (i && ((x[(size_t)i - 1] < 0.f) != (x[(size_t)i] < 0.f))) ++zc;
        }
        v[0] = (float)std::sqrt(e / N);
        v[7] = (float)(zc * sr / N);
        // spectrum: Hann window, amplitudes scaled so that a full-scale sine is 1
        std::vector<float> re((size_t)2 * N, 0.f), im((size_t)2 * N, 0.f);
        for (int i = 0; i < N; ++i) re[(size_t)i] = x[(size_t)i] * (float)(.5 - .5 * std::cos(dsp::kTwoPi * i / N));
        musil::dsp::FFT<float> f;
        f.init(N);
        f.forward(re.data(), im.data());
        std::vector<float> amp((size_t)H + 1);
        for (int k = 0; k <= H; ++k) amp[(size_t)k] = std::hypot(re[(size_t)k], im[(size_t)k]) * 4.f / N;
        double sa = 0, sf = 0;
        for (int k = 0; k <= H; ++k) { sa += amp[(size_t)k]; sf += amp[(size_t)k] * (k * sr / N); }
        double cen = sa > 0 ? sf / sa : 0, m2 = 0, m3 = 0, m4 = 0;
        for (int k = 0; k <= H && sa > 0; ++k) {
            double d = k * sr / N - cen, a = amp[(size_t)k] / sa;
            m2 += a * d * d; m3 += a * d * d * d; m4 += a * d * d * d * d;
        }
        double spread = std::sqrt(m2);
        v[1] = (float)cen;
        v[2] = (float)spread;
        v[3] = spread > 0 ? (float)(m3 / (spread * spread * spread)) : 0.f;
        v[4] = spread > 0 ? (float)(m4 / (spread * spread * spread * spread)) : 0.f;
        double flux = 0, hfc = 0, hn = 0, logs = 0;
        for (int k = 0; k <= H; ++k) {
            float a = amp[(size_t)k], d = a - prevAmp_[(size_t)k];
            flux += d > 0 ? d : 0;
            hfc += (double)a * a * k;
            hn += k;
            logs += std::log(a + 1e-12);
        }
        prevAmp_ = amp;
        v[5] = (float)flux;
        v[6] = (float)(hn > 0 ? hfc / hn : 0);
        double mean = sa / (H + 1);
        v[8] = mean > 1e-9 ? (float)(std::exp(logs / (H + 1)) / mean) : 0.f;
        // autocorrelation by FFT (zero-padded to 2N, so not circular)
        std::fill(re.begin(), re.end(), 0.f);
        std::fill(im.begin(), im.end(), 0.f);
        for (int i = 0; i < N; ++i) re[(size_t)i] = x[(size_t)i];
        fft_.forward(re.data(), im.data());
        for (size_t k = 0; k < re.size(); ++k) { re[k] = re[k] * re[k] + im[k] * im[k]; im[k] = 0.f; }
        fft_.inverse(re.data(), im.data());
        // acfF0Estimate: skip the first lobe, take the highest peak after it
        std::vector<float> r((size_t)H);
        for (int i = 0; i < H; ++i) r[(size_t)i] = re[(size_t)i] * (float)(H - i) / N;   // its unbiasing
        int j = (int)(N * .02);
        float t = r[0];
        while (j < H && r[(size_t)j] < t) t = r[(size_t)j++];
        t = 0.f;
        for (int i = j; i < H; ++i)
            if (r[(size_t)i] > t) { j = i; t = r[(size_t)i]; }
        float per = (r[0] > 0 && j < H) ? r[(size_t)j] / r[0] : 0.f;
        v[9] = std::clamp(per, 0.f, 1.f);
        v[10] = (j > 0 && j < H && per >= .4f && j <= N / 4) ? (float)(sr / j) : 0.f;
        for (int k = (int)which_.size() - 1; k >= 0; --k) outletFloat(k, v[which_[(size_t)k]]);
    }

    std::unique_ptr<std::atomic<float>[]> ring_;
    std::atomic<size_t> wpos_{0};
    int n_, hop_;
    std::vector<int> which_;
    std::vector<float> prevAmp_;
    musil::dsp::FFT<float> fft_;
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
        else if (h.is("seed")) seed_.store((unsigned)num(1, 1));
        else if (h.is("window") && m.size() > 1) {
            std::lock_guard<std::mutex> lk(patch->mutex());
            neb_.setWindow(m[1].str());
        } else if ((h.is("pitch1") || h.is("pitch2")) && m.size() > 1) {
            pitch_[h.is("pitch1") ? 0 : 1].store(std::max(0.f, num(1, 0)));
        } else if ((h.is("proto1") || h.is("proto2")) && m.size() > 1) {
            std::lock_guard<std::mutex> lk(patch->mutex());
            int k = h.is("proto1") ? 0 : 1;
            names_[k] = m[1].str();
            if (!neb_.setWave(k, names_[k])) neb_.setSound(k, nullptr, 0, 1, 44100.0);
            buf_[k].set(patch, Neb::isWave(names_[k]) ? "" : names_[k]);
        }
    }
    void dsp(int n) override {
        if (!audioBuffersAvailable()) {        // the GUI holds the lock: it is changing neb_ (proto, window)
            dsp::fill(sigOut(0), 0.f, n);
            dsp::fill(sigOut(1), 0.f, n);
            return;
        }
        if (unsigned s = seed_.exchange(0)) neb_.seed(s);
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
    std::atomic<unsigned> seed_{0};
};

}  // namespace

void registerSoundObjects(ClassTable& t) {
    t.insert(t.end(), {
        {"sparkle~", "Spectral", "phase vocoder: [buffer] [channels] [window] [overlap]", with<SparkleBox>()},
        {"conv~", "Filters", "zero-latency convolution with a buffer~: buffer [channels]", with<ConvBox>()},
        {"condec", "Samples", "convolve / deconvolve buffers: conv|deconv x h out", plain<CondecBox>()},
        {"descriptors~", "Analysis", "low-level descriptors and f0: [size] [hop] [names]", with<DescriptorsBox>()},
        {"function", "Interface", "breakpoint editor: [domain ms] [lo] [hi]", with<FunctionBox>()},
        {"nebula~", "Oscillators", "granular synthesis (sound_math's nebula): [proto1] [proto2]", with<NebulaBox>()},
    });
}

}  // namespace moritz
