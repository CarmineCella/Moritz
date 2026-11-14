// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_samples.cpp — buffer~, play~, groove~, record~, sfplay~ and analysis

#include "objects.h"

#include <FL/fl_draw.H>

#include <atomic>

namespace mlsp {

bool decodeAudioFile(const std::string& path, SampleBuffer& out, std::string& err) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder dec;
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) {
        err = "can't read audio file " + path;
        return false;
    }
    SampleBuffer b;
    b.channels = (int)dec.outputChannels;
    b.sr = dec.outputSampleRate;
    std::vector<float> chunk(4096 * (size_t)b.channels);
    for (;;) {
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&dec, chunk.data(), 4096, &got);
        b.data.insert(b.data.end(), chunk.begin(), chunk.begin() + (long)(got * b.channels));
        if (r != MA_SUCCESS || got == 0) break;
    }
    ma_decoder_uninit(&dec);
    b.frames = (long)(b.data.size() / b.channels);
    out = std::move(b);
    return true;
}

bool writeWavFile(const std::string& path, const SampleBuffer& b, std::string& err) {
    ma_encoder_config cfg =
        ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, (ma_uint32)b.channels,
                               (ma_uint32)b.sr);
    ma_encoder enc;
    if (ma_encoder_init_file(path.c_str(), &cfg, &enc) != MA_SUCCESS) {
        err = "can't write " + path;
        return false;
    }
    std::vector<int16_t> pcm(b.data.size());
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = (int16_t)std::lrint(std::clamp(b.data[i], -1.f, 1.f) * 32767.f);
    ma_encoder_write_pcm_frames(&enc, pcm.data(), (ma_uint64)b.frames, nullptr);
    ma_encoder_uninit(&enc);
    return true;
}


BufferBox* findBuffer(Patch* p, const std::string& name) {
    BufferBox* found = nullptr;
    p->root()->forEachBox([&](Box* b) {
        if (auto bb = dynamic_cast<BufferBox*>(b))
            if (!found && bb->name == name) found = bb;
    });
    return found;
}

namespace {


// ---------------------------------------------------------------- play~
// play~ name [channels]: plays a buffer~. With a signal in the inlet it
// reads at that position (ms, e.g. from line~); otherwise "start [from
// [to]]" (ms) or bang plays it, "stop" stops.
class PlayBox : public Box {
public:
    explicit PlayBox(const Message& a) : name_(argstr(a, 0)) {
        setPorts({P::Signal}, std::vector<Port>((size_t)std::clamp((int)argf(a, 1, 1.f), 1, 8), P::Signal));
    }
    void resolve() override { b_ = findBuffer(patch, name_); }
    void message(int, const Message& m) override {
        if (m[0].is("set") && m.size() > 1) { name_ = m[1].str(); patch->rebuild(); return; }
        if (m[0].is("stop")) { cmd_.store(2); return; }
        if (m[0].is("bang") || m[0].is("start")) {
            startMs_.store(m.size() > 1 && m[1].isFloat() ? m[1].f : 0.f);
            endMs_.store(m.size() > 2 && m[2].isFloat() ? m[2].f : -1.f);
            cmd_.store(1);
        }
    }
    void dsp(int n) override {
        int c = cmd_.exchange(0);
        const SampleBuffer* b = b_ ? &b_->buf : nullptr;
        double ms2fr = b ? b->sr / 1000.0 : 0.0;
        if (c == 1 && b) {
            pos_ = startMs_.load() * ms2fr;
            float e = endMs_.load();
            end_ = e > 0 ? e * ms2fr : (double)b->frames;
            playing_ = true;
        } else if (c == 2) {
            playing_ = false;
        }
        bool driven = !inSrc[0].empty();
        const float* posMs = driven ? sigIn(0, n) : nullptr;
        double rate = b ? b->sr / patch->root()->sampleRate : 0.0;
        for (int k = 0; k < numOuts(); ++k) {
            float* y = sigOut(k);
            for (int i = 0; i < n; ++i) {
                double p = driven ? posMs[i] * ms2fr : pos_ + i * rate;
                y[i] = (b && (driven || (playing_ && p < end_))) ? b->interp(p, k) : 0.f;
            }
        }
        if (playing_ && !driven) {
            pos_ += n * rate;
            if (pos_ >= end_) playing_ = false;
        }
    }

private:
    std::string name_;
    BufferBox* b_ = nullptr;
    std::atomic<int> cmd_{0};
    std::atomic<float> startMs_{0.f}, endMs_{-1.f};
    double pos_ = 0.0, end_ = 0.0;
    bool playing_ = false;
};

// ---------------------------------------------------------------- groove~
// groove~ name [channels]: variable-speed looping playback. Left inlet:
// playback rate (signal, e.g. sig~ 1); a float there jumps to that position
// (ms). Inlets 2/3: loop start/end (ms, 0 end = whole buffer). "loop 1"
// turns looping on. Last outlet: sync signal (0..1 through the loop).
class GrooveBox : public Box {
public:
    explicit GrooveBox(const Message& a) : name_(argstr(a, 0)) {
        int ch = std::clamp((int)argf(a, 1, 1.f), 1, 8);
        std::vector<Port> outs((size_t)ch + 1, P::Signal);
        setPorts({P::Signal, P::Control, P::Control}, outs);
    }
    void resolve() override { b_ = findBuffer(patch, name_); }
    void message(int in, const Message& m) override {
        if (in > 0) { if (m[0].isFloat()) setScalar(in, std::max(0.f, m[0].f)); return; }
        if (m[0].is("loop") && m.size() > 1) loop_.store(m[1].isFloat() && m[1].f != 0.f);
        else if (m[0].is("set") && m.size() > 1) { name_ = m[1].str(); patch->rebuild(); }
        else if (m[0].is("startloop")) jumpMs_.store(scalar(1));
        else if (m[0].isFloat()) jumpMs_.store(std::max(0.f, m[0].f));
    }
    void dsp(int n) override {
        int ch = numOuts() - 1;
        float* sync = sigOut(ch);
        const SampleBuffer* b = b_ ? &b_->buf : nullptr;
        if (!b || b->frames < 2) {
            for (int k = 0; k <= ch; ++k) dsp::fill(sigOut(k), 0.f, n);
            return;
        }
        double ms2fr = b->sr / 1000.0, ratio = b->sr / patch->root()->sampleRate;
        float j = jumpMs_.exchange(-1.f);
        if (j >= 0.f) pos_ = j * ms2fr;
        double ls = std::clamp(scalar(1) * ms2fr, 0.0, (double)b->frames - 1);
        double le = scalar(2) > 0.f ? std::min(scalar(2) * ms2fr, (double)b->frames) : (double)b->frames;
        if (le <= ls + 1) le = (double)b->frames;
        bool loop = loop_.load();
        const float* rate = sigIn(0, n);
        for (int i = 0; i < n; ++i) {
            if (loop) {
                double L = le - ls;
                if (pos_ >= le) pos_ = ls + std::fmod(pos_ - ls, L);
                if (pos_ < ls) pos_ = le - std::fmod(ls - pos_, L);
            }
            bool inside = pos_ >= 0 && pos_ < b->frames;
            for (int k = 0; k < ch; ++k) sigOut(k)[i] = inside ? b->interp(pos_, k) : 0.f;
            sync[i] = (float)std::clamp((pos_ - ls) / (le - ls), 0.0, 1.0);
            pos_ += rate[i] * ratio;
            if (!loop) pos_ = std::clamp(pos_, -1.0, (double)b->frames);
        }
    }

private:
    std::string name_;
    BufferBox* b_ = nullptr;
    std::atomic<bool> loop_{false};
    std::atomic<float> jumpMs_{-1.f};
    double pos_ = 0.0;
};

// ---------------------------------------------------------------- record~
// record~ name [channels]: records its signal inlets into a buffer~.
// 1 starts (from the beginning), 0 stops; "loop 1" wraps instead of
// stopping at the end. Outlet: sync signal (0..1 through the buffer).
class RecordBox : public Box {
public:
    explicit RecordBox(const Message& a) : name_(argstr(a, 0)) {
        int ch = std::clamp((int)argf(a, 1, 1.f), 1, 8);
        setPorts(std::vector<Port>((size_t)ch, P::Signal), {P::Signal});
    }
    void resolve() override { b_ = findBuffer(patch, name_); }
    void message(int in, const Message& m) override {
        if (in != 0) return;
        if (m[0].is("loop") && m.size() > 1) loop_.store(m[1].isFloat() && m[1].f != 0.f);
        else if (m[0].is("set") && m.size() > 1) { name_ = m[1].str(); patch->rebuild(); }
        else if (m[0].isFloat()) cmd_.store(m[0].f != 0.f ? 1 : 2);
    }
    void dsp(int n) override {
        int c = cmd_.exchange(0);
        if (c == 1) { pos_ = 0; rec_ = true; }
        if (c == 2) rec_ = false;
        SampleBuffer* b = b_ ? &b_->buf : nullptr;
        float* sync = sigOut(0);
        std::vector<const float*> in((size_t)numIns());
        for (int k = 0; k < numIns(); ++k) in[(size_t)k] = sigIn(k, n);
        for (int i = 0; i < n; ++i) {
            if (rec_ && b && b->frames > 0) {
                if (pos_ >= b->frames) {
                    if (loop_.load()) pos_ = 0;
                    else rec_ = false;
                }
                if (rec_) {
                    for (int k = 0; k < b->channels; ++k)
                        b->data[(size_t)pos_ * b->channels + k] = in[(size_t)(k % numIns())][i];
                    ++pos_;
                }
            }
            sync[i] = b && b->frames > 0 ? (float)pos_ / (float)b->frames : 0.f;
        }
    }

private:
    std::string name_;
    BufferBox* b_ = nullptr;
    std::atomic<int> cmd_{0};
    std::atomic<bool> loop_{false};
    long pos_ = 0;
    bool rec_ = false;
};

// ---------------------------------------------------------------- sfplay~
// sfplay~ [channels]: "open file" loads a sound file; 1 plays from the
// start, 0 stops; "loop 1" loops; "speed x" sets the playback speed.
class SfplayBox : public Box {
public:
    explicit SfplayBox(const Message& a) {
        int ch = std::clamp((int)argf(a, 0, 1.f), 1, 8);
        setPorts({P::Control}, std::vector<Port>((size_t)ch, P::Signal));
    }
    void message(int, const Message& m) override {
        if (m[0].is("open") && m.size() > 1) {
            SampleBuffer b;
            std::string err;
            if (!decodeAudioFile(patch->resolvePath(m[1].str()), b, err)) { patch->print("sfplay~: " + err); return; }
            std::lock_guard<std::mutex> lk(patch->mutex());
            buf_ = std::move(b);
            playing_ = false;
        } else if (m[0].is("loop") && m.size() > 1) {
            loop_.store(m[1].isFloat() && m[1].f != 0.f);
        } else if (m[0].is("speed") && m.size() > 1 && m[1].isFloat()) {
            speed_.store(m[1].f);
        } else if (m[0].isFloat()) {
            cmd_.store(m[0].f != 0.f ? 1 : 2);
        } else if (m[0].is("bang")) {
            cmd_.store(1);
        }
    }
    void dsp(int n) override {
        int c = cmd_.exchange(0);
        if (c == 1) { pos_ = 0; playing_ = buf_.frames > 0; }
        if (c == 2) playing_ = false;
        double step = buf_.sr / patch->root()->sampleRate * speed_.load();
        for (int k = 0; k < numOuts(); ++k) {
            float* y = sigOut(k);
            double p = pos_;
            for (int i = 0; i < n; ++i) {
                if (playing_ && loop_.load() && buf_.frames > 0) p = std::fmod(p, (double)buf_.frames);
                y[i] = playing_ ? buf_.interp(p, k) : 0.f;
                p += step;
            }
        }
        if (playing_) {
            pos_ += n * step;
            if (loop_.load() && buf_.frames > 0) pos_ = std::fmod(pos_, (double)buf_.frames);
            else if (pos_ >= buf_.frames || pos_ < 0) playing_ = false;
        }
    }

private:
    SampleBuffer buf_;
    std::atomic<int> cmd_{0};
    std::atomic<bool> loop_{false};
    std::atomic<float> speed_{1.f};
    double pos_ = 0.0;
    bool playing_ = false;
};



// ================================================================ analysis

// ---------------------------------------------------------------- snapshot~ / peakamp~
// snapshot~ [ms]: outputs the signal's current value on bang, or every ms.
// peakamp~ [ms]: outputs the peak absolute value since the last report.
class ProbeBox : public Box {
public:
    ProbeBox(bool peak, const Message& a) : peak_(peak), interval_(std::max(0.f, argf(a, 0, 0.f))) {
        setPorts({P::Signal, P::Control}, {P::Control});
    }
    void attached() override { restart(); }
    void message(int in, const Message& m) override {
        if (in == 1 || (m[0].is("interval") && m.size() > 1)) {
            const Atom& v = in == 1 ? m[0] : m[1];
            if (v.isFloat()) { interval_ = std::max(0.f, v.f); restart(); }
            return;
        }
        if (m[0].is("bang")) report();
        else if (m[0].isFloat()) { interval_ = std::max(0.f, m[0].f); restart(); }
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        if (peak_) {
            float p = 0.f;
            for (int i = 0; i < n; ++i) p = std::max(p, std::fabs(x[i]));
            if (p > value_.load(std::memory_order_relaxed)) value_.store(p, std::memory_order_relaxed);
        } else {
            value_.store(x[n - 1], std::memory_order_relaxed);
        }
    }

private:
    void report() { outletFloat(0, peak_ ? value_.exchange(0.f) : value_.load()); }
    void restart() {
        patch->cancel(this);
        if (interval_ > 0.f) tick();
    }
    void tick() {
        if (patch->root()->dspRunning) report();
        patch->schedule(this, std::max(5.f, interval_) / 1000.0, [this] { tick(); });
    }
    bool peak_;
    float interval_;
    std::atomic<float> value_{0.f};
};

// ---------------------------------------------------------------- meter~
class MeterBox : public Box {
public:
    MeterBox() { kind = BoxKind::UI; setPorts({P::Signal}, {}); }
    void uiSize(int& w, int& h) const override { w = 18; h = 90; }
    bool animated() const override { return true; }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float p = 0.f;
        for (int i = 0; i < n; ++i) p = std::max(p, std::fabs(x[i]));
        if (p > peak_.load(std::memory_order_relaxed)) peak_.store(p, std::memory_order_relaxed);
    }
    void drawUI(int X, int Y) override {
        level_ = std::max(peak_.exchange(0.f), level_ * 0.85f);
        double db = 20.0 * std::log10(level_ + 1e-9);
        double norm = std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
        int H = h - 8, bh = (int)(H * norm);
        fl_color(fl_rgb_color(26, 27, 31));
        fl_rectf(X + 4, Y + 4, w - 8, H);
        Fl_Color c = db > -3 ? fl_rgb_color(235, 80, 70)
                   : db > -12 ? fl_rgb_color(235, 200, 70) : fl_rgb_color(90, 200, 120);
        fl_color(c);
        fl_rectf(X + 4, Y + 4 + H - bh, w - 8, bh);
    }

private:
    std::atomic<float> peak_{0.f};
    float level_ = 0.f;
};

// ---------------------------------------------------------------- scope~
// scope~ [samples]: oscilloscope, triggered on rising zero crossings.
class ScopeBox : public Box {
public:
    explicit ScopeBox(const Message& a) : ring_(new std::atomic<float>[kRing]) {
        kind = BoxKind::UI;
        setPorts({P::Signal}, {});
        window_ = std::clamp((int)argf(a, 0, 512.f), 32, kRing / 4);
        for (int i = 0; i < kRing; ++i) ring_[i].store(0.f);
    }
    void uiSize(int& w, int& h) const override { w = 180; h = 100; }
    bool animated() const override { return true; }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        size_t w = w_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) ring_[(w + i) & (kRing - 1)].store(x[i], std::memory_order_relaxed);
        w_.store(w + n, std::memory_order_release);
    }
    void drawUI(int X, int Y) override {
        fl_color(fl_rgb_color(20, 24, 22));
        fl_rectf(X + 3, Y + 3, w - 6, h - 6);
        fl_color(fl_rgb_color(45, 60, 52));
        fl_xyline(X + 3, Y + h / 2, X + w - 4);
        size_t end = w_.load(std::memory_order_acquire);
        size_t N = (size_t)window_;
        size_t start = end > 2 * N ? end - 2 * N : 0;
        auto s = [&](size_t i) { return ring_[i & (kRing - 1)].load(std::memory_order_relaxed); };
        size_t trig = end > N ? end - N : 0;
        for (size_t i = start + 1; i + N <= end && i < start + N; ++i)
            if (s(i - 1) < 0.f && s(i) >= 0.f) { trig = i; break; }
        fl_color(kSignal);
        fl_push_clip(X + 3, Y + 3, w - 6, h - 6);
        fl_begin_line();
        double sx = (double)(w - 8) / (double)(N - 1), sy = (h - 10) / 2.0;
        for (size_t i = 0; i < N; ++i)
            fl_vertex(X + 4 + i * sx, Y + h / 2.0 - std::clamp(s(trig + i), -1.f, 1.f) * sy);
        fl_end_line();
        fl_pop_clip();
    }

private:
    static constexpr int kRing = 4096;
    std::unique_ptr<std::atomic<float>[]> ring_;
    std::atomic<size_t> w_{0};
    int window_;
};



}  // namespace

void registerSampleObjects(ClassTable& t) {
    using M = const Message&;
    t.insert(t.end(), {
        {"buffer~", "Samples", "named sample memory", with<BufferBox>()},
        {"play~", "Samples", "play a buffer~", with<PlayBox>()},
        {"groove~", "Samples", "variable-speed looping playback", with<GrooveBox>()},
        {"record~", "Samples", "record into a buffer~", with<RecordBox>()},
        {"sfplay~", "Samples", "play a sound file", with<SfplayBox>()},
        {"snapshot~", "Analysis", "signal value as a number", [](M a, Patch*) { return std::make_unique<ProbeBox>(false, a); }},
        {"peakamp~", "Analysis", "peak amplitude as a number", [](M a, Patch*) { return std::make_unique<ProbeBox>(true, a); }},
        {"meter~", "Analysis", "level meter", plain<MeterBox>()},
        {"scope~", "Analysis", "oscilloscope", with<ScopeBox>()},
    });
}

}  // namespace mlsp
