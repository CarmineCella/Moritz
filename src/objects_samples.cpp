// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_samples.cpp — buffer~, play~, groove~, record~, sfplay~ and analysis

#include "objects.h"

#include <FL/fl_draw.H>

#include <atomic>
#include <chrono>
#include <thread>

namespace moritz {

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
        if (c == 1) { pos_ = speed_.load() < 0.f ? buf_.frames - 1.0 : 0.0; playing_ = buf_.frames > 0; }
        if (c == 2) playing_ = false;
        double step = buf_.sr / patch->root()->sampleRate * speed_.load();
        for (int k = 0; k < numOuts(); ++k) {
            float* y = sigOut(k);
            double p = pos_;
            for (int i = 0; i < n; ++i) {
                if (playing_ && loop_.load() && buf_.frames > 0) p = wrap(p);
                y[i] = playing_ ? buf_.interp(p, k) : 0.f;
                p += step;
            }
        }
        if (playing_) {
            pos_ += n * step;
            if (loop_.load() && buf_.frames > 0) pos_ = wrap(pos_);
            else if (pos_ >= buf_.frames || pos_ < 0) playing_ = false;
        }
    }
    double wrap(double p) const {            // into 0..frames, also for negative speeds
        p = std::fmod(p, (double)buf_.frames);
        return p < 0 ? p + buf_.frames : p;
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



// ---------------------------------------------------------------- sfrecord~
// sfrecord~ [channels]: records its signal inlets (1 by default) to a 24-bit
// WAV file. "open file.wav" names the file (relative to the patch); 1 starts
// and 0 stops, and the file is complete once it stops. The outlet reports the
// recorded time in ms, every 250 ms and when it stops. Audio must be on.
// The audio thread only copies samples into a ring; a writer thread empties
// it into the file, so recording never blocks the audio.
class SfrecordBox : public Box {
public:
    explicit SfrecordBox(const Message& a) : ring_(new float[kRing]) {
        ch_ = std::clamp((int)argf(a, 0, 1.f), 1, 8);
        setPorts(std::vector<Port>((size_t)ch_, P::Signal), {P::Control});
    }
    ~SfrecordBox() override { stop(false, false); }
    void message(int in, const Message& m) override {
        if (in > 0) { if (m[0].isFloat()) setScalar(in, m[0].f); return; }
        if (m[0].is("open") && m.size() > 1) path_ = patch->resolvePath(m[1].str());
        else if (m[0].isFloat()) m[0].f != 0.f ? start() : stop(true, true);
        else if (m[0].is("start")) start();
        else if (m[0].is("stop")) stop(true, true);
    }
    void dsp(int n) override {
        if (!recording_.load(std::memory_order_acquire)) return;
        size_t need = (size_t)n * (size_t)ch_;
        size_t w = w_.load(std::memory_order_relaxed), r = r_.load(std::memory_order_acquire);
        if (kRing - (w - r) < need) { dropped_ += (uint64_t)n; return; }   // the writer fell behind
        for (int c = 0; c < ch_; ++c) {
            const float* x = sigIn(c, n);
            for (int i = 0; i < n; ++i) ring_[(w + (size_t)i * ch_ + c) & (kRing - 1)] = x[i];
        }
        w_.store(w + need, std::memory_order_release);
        frames_ += (uint64_t)n;
    }

private:
    static constexpr size_t kRing = 1 << 20;     // floats: ~10 s of stereo at 48 kHz

    void start() {
        if (recording_) return;
        if (path_.empty()) { patch->print("sfrecord~: no file; send \"open name.wav\" first"); return; }
        sr_ = patch->root()->sampleRate.load();
        ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s24,
                                                       (ma_uint32)ch_, (ma_uint32)sr_);
        if (ma_encoder_init_file(path_.c_str(), &cfg, &enc_) != MA_SUCCESS) {
            patch->print("sfrecord~: can't write " + path_);
            return;
        }
        if (!patch->root()->dspRunning) patch->print("sfrecord~: audio is off; it records once audio starts");
        r_ = 0;
        w_ = 0;
        frames_ = 0;
        dropped_ = 0;
        writing_ = true;
        writer_ = std::thread([this] { writeLoop(); });
        recording_.store(true, std::memory_order_release);
        patch->schedule(this, 0.25, [this] { tick(); });
    }
    // lock: wait for the audio thread to leave dsp() (not when the patch is
    // already locked, e.g. while the box is being deleted)
    void stop(bool report, bool lock) {
        if (!writer_.joinable()) return;
        recording_.store(false, std::memory_order_release);
        if (lock) { std::lock_guard<std::mutex> lk(patch->mutex()); }
        writing_ = false;
        writer_.join();
        ma_encoder_uninit(&enc_);
        if (!report) return;
        patch->cancel(this);
        outletFloat(0, ms());
        if (dropped_ > 0)
            patch->print("sfrecord~: " + std::to_string(dropped_.load()) + " samples lost (disk too slow)");
    }
    void tick() {
        if (!recording_) return;
        outletFloat(0, ms());
        patch->schedule(this, 0.25, [this] { tick(); });
    }
    float ms() const { return (float)(frames_.load() / sr_ * 1000.0); }
    void writeLoop() {
        std::vector<uint8_t> pcm;
        for (;;) {
            size_t r = r_.load(std::memory_order_relaxed), w = w_.load(std::memory_order_acquire);
            size_t avail = w - r;
            if (avail == 0) {
                if (!writing_) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            size_t chunk = std::min<size_t>(avail, 1 << 16);
            chunk -= chunk % (size_t)ch_;
            pcm.resize(chunk * 3);
            for (size_t i = 0; i < chunk; ++i) {
                float x = ring_[(r + i) & (kRing - 1)];
                int32_t v = (int32_t)std::lrint(std::clamp(std::isfinite(x) ? x : 0.f, -1.f, 1.f) * 8388607.f);
                pcm[i * 3] = (uint8_t)(v & 0xff);
                pcm[i * 3 + 1] = (uint8_t)((v >> 8) & 0xff);
                pcm[i * 3 + 2] = (uint8_t)((v >> 16) & 0xff);
            }
            ma_encoder_write_pcm_frames(&enc_, pcm.data(), (ma_uint64)(chunk / (size_t)ch_), nullptr);
            r_.store(r + chunk, std::memory_order_release);
        }
    }

    int ch_;
    std::string path_;
    double sr_ = 44100.0;
    std::unique_ptr<float[]> ring_;
    std::atomic<size_t> r_{0}, w_{0};
    std::atomic<uint64_t> frames_{0}, dropped_{0};
    std::atomic<bool> recording_{false}, writing_{false};
    std::thread writer_;
    ma_encoder enc_;
};

// ---------------------------------------------------------------- waveform~
// waveform~ name [width height]: shows a buffer~, one lane per channel. In
// locked mode, drag across it to select a region; the outlets give the start
// and end in ms. Inlet: a number draws a cursor there (ms), "select start
// end" sets the selection and outputs it, "set name" shows another buffer~.
class WaveformBox : public Box {
public:
    explicit WaveformBox(const Message& a) : name_(argstr(a, 0)) {
        kind = BoxKind::UI;
        setPorts({P::Control}, {P::Control, P::Control});
        w_ = std::clamp((int)argf(a, 1, 320.f), 80, 2000);
        h_ = std::clamp((int)argf(a, 2, 110.f), 40, 1000);
    }
    void uiSize(int& w, int& h) const override { w = w_; h = h_; }
    bool animated() const override { return true; }   // follows record~ while it writes
    void resolve() override { b_ = findBuffer(patch, name_); }
    void message(int, const Message& m) override {
        if (m[0].isFloat()) { cursor_ = m[0].f; patch->redraw(); }
        else if (m[0].is("set") && m.size() > 1) { name_ = m[1].str(); b_ = findBuffer(patch, name_); patch->redraw(); }
        else if (m[0].is("select") && m.size() > 2) { s0_ = argf(m, 1, 0.f); s1_ = argf(m, 2, 0.f); output(); }
    }
    void uiMouse(int e, int mx, int) override {
        float t = msAt(mx);
        if (e == FL_PUSH) { anchor_ = t; s0_ = s1_ = t; }
        else { s0_ = std::min(anchor_, t); s1_ = std::max(anchor_, t); }
        output();
    }
    void drawUI(int X, int Y) override {
        int x0 = X + 3, y0 = Y + 3, W = w - 6, H = h - 6;
        fl_color(fl_rgb_color(22, 24, 28));
        fl_rectf(x0, y0, W, H);
        const SampleBuffer* sb = b_ ? &b_->buf : nullptr;
        fl_font(kFont, 9);
        if (!sb || sb->frames <= 0) {
            fl_color(kDim);
            fl_draw(("no buffer~ " + name_).c_str(), x0 + 6, y0 + H / 2 + 3);
            return;
        }
        double dur = sb->frames / sb->sr * 1000.0;
        if (s1_ > s0_) {                                    // selection behind the wave
            int a = x0 + (int)(std::clamp(s0_ / dur, 0.0, 1.0) * W);
            int b = x0 + (int)(std::clamp(s1_ / dur, 0.0, 1.0) * W);
            fl_color(fl_rgb_color(40, 58, 88));
            fl_rectf(a, y0, std::max(1, b - a), H);
        }
        int ch = sb->channels, laneH = H / ch;
        fl_color(kSignal);
        for (int c = 0; c < ch; ++c) {
            int mid = y0 + c * laneH + laneH / 2;
            double half = laneH / 2.0 - 2;
            for (int px = 0; px < W; ++px) {
                long f0 = (long)((double)px / W * sb->frames), f1 = (long)((double)(px + 1) / W * sb->frames);
                f1 = std::max(f1, f0 + 1);
                long stride = std::max(1L, (f1 - f0) / 128);
                float lo = 1.f, hi = -1.f;
                for (long f = f0; f < f1 && f < sb->frames; f += stride) {
                    float v = sb->data[(size_t)f * ch + c];
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                if (hi < lo) continue;
                int ya = mid - (int)(std::clamp(hi, -1.f, 1.f) * half), yb = mid - (int)(std::clamp(lo, -1.f, 1.f) * half);
                fl_yxline(x0 + px, ya, std::max(ya, yb));
            }
        }
        if (cursor_ >= 0.f && cursor_ <= dur) {
            fl_color(kAccent);
            fl_yxline(x0 + (int)(cursor_ / dur * W), y0, y0 + H - 1);
        }
        fl_color(kDim);
        char label[96];
        std::snprintf(label, sizeof label, "%s  %.0f ms", name_.c_str(), dur);
        fl_draw(label, x0 + 4, y0 + 10);
    }

private:
    float msAt(int mx) const {
        if (!b_ || b_->buf.frames <= 0) return 0.f;
        double dur = b_->buf.frames / b_->buf.sr * 1000.0;
        return (float)(std::clamp((mx - 3.0) / (w - 6.0), 0.0, 1.0) * dur);
    }
    void output() {
        patch->redraw();
        outletFloat(1, s1_);
        outletFloat(0, s0_);
    }
    std::string name_;
    BufferBox* b_ = nullptr;
    int w_, h_;
    float cursor_ = -1.f, s0_ = 0.f, s1_ = 0.f, anchor_ = 0.f;
};

// ---------------------------------------------------------------- spectroscope~
// spectroscope~ [width height]: the live spectrum of its signal: a 2048-point
// FFT with a Hann window, frequency on a log scale from 20 Hz to half the
// sample rate, level from 0 dB (a full-scale sine) down to -90 dB. Peaks
// fall back slowly, so short sounds stay visible for a moment.
class SpectroscopeBox : public Box {
public:
    explicit SpectroscopeBox(const Message& a) : ring_(new std::atomic<float>[kRing]) {
        kind = BoxKind::UI;
        setPorts({P::Signal}, {});
        w_ = std::clamp((int)argf(a, 0, 300.f), 120, 2000);
        h_ = std::clamp((int)argf(a, 1, 130.f), 60, 1000);
        for (int i = 0; i < kRing; ++i) ring_[i].store(0.f);
        fft_.init(kN);
    }
    void uiSize(int& w, int& h) const override { w = w_; h = h_; }
    bool animated() const override { return true; }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        size_t w = wpos_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) ring_[(w + i) & (kRing - 1)].store(x[i], std::memory_order_relaxed);
        wpos_.store(w + n, std::memory_order_release);
    }
    void drawUI(int X, int Y) override {
        int x0 = X + 3, y0 = Y + 3, W = w - 6, H = h - 6;
        fl_color(fl_rgb_color(20, 22, 26));
        fl_rectf(x0, y0, W, H);
        double sr = patch->root()->sampleRate.load(), nyq = sr / 2;
        auto xOf = [&](double f) { return x0 + (int)(std::log(f / 20.0) / std::log(nyq / 20.0) * W); };
        auto yOf = [&](double db) { return y0 + (int)(std::clamp(-db / 90.0, 0.0, 1.0) * (H - 1)); };
        for (double f : {100.0, 1000.0, 10000.0}) {         // grid
            if (f >= nyq) continue;
            fl_color(fl_rgb_color(42, 46, 54));
            fl_yxline(xOf(f), y0, y0 + H - 1);
        }
        for (double db : {-30.0, -60.0}) {
            fl_color(fl_rgb_color(42, 46, 54));
            fl_xyline(x0, yOf(db), x0 + W - 1);
        }
        // spectrum of the latest kN samples
        size_t end = wpos_.load(std::memory_order_acquire);
        std::vector<float> re(kN), im(kN, 0.f);
        for (int i = 0; i < kN; ++i) {
            float win = 0.5f - 0.5f * (float)std::cos(dsp::kTwoPi * i / kN);
            re[(size_t)i] = win * ring_[(end - kN + (size_t)i) & (kRing - 1)].load(std::memory_order_relaxed);
        }
        fft_.forward(re.data(), im.data());
        if ((int)disp_.size() != W) disp_.assign((size_t)W, -200.f);
        for (int px = 0; px < W; ++px) {
            double fa = 20.0 * std::pow(nyq / 20.0, (double)px / W), fb = 20.0 * std::pow(nyq / 20.0, (double)(px + 1) / W);
            int ka = std::max(1, (int)(fa / sr * kN)), kb = std::max(ka, std::min(kN / 2 - 1, (int)(fb / sr * kN)));
            float m = 0.f;
            for (int k = ka; k <= kb; ++k) m = std::max(m, re[(size_t)k] * re[(size_t)k] + im[(size_t)k] * im[(size_t)k]);
            float db = 10.f * std::log10(m * 16.f / ((float)kN * kN) + 1e-20f);   // 0 dB = full-scale sine
            disp_[(size_t)px] = std::max(db, disp_[(size_t)px] - 1.5f);
        }
        fl_color(fl_color_average(kSignal, fl_rgb_color(20, 22, 26), 0.35f));
        for (int px = 0; px < W; ++px) fl_yxline(x0 + px, yOf(disp_[(size_t)px]), y0 + H - 1);
        fl_color(kSignal);
        fl_begin_line();
        for (int px = 0; px < W; ++px) fl_vertex(x0 + px, yOf(disp_[(size_t)px]));
        fl_end_line();
        fl_font(kFont, 9);                                  // labels on top
        fl_color(kText);
        for (double f : {100.0, 1000.0, 10000.0})
            if (f < nyq) fl_draw(f >= 1000 ? (std::to_string((int)(f / 1000)) + "k").c_str() : "100", xOf(f) + 2, y0 + 10);
        fl_draw("-30", x0 + 2, yOf(-30) - 2);
        fl_draw("-60", x0 + 2, yOf(-60) - 2);
    }

private:
    static constexpr int kRing = 8192, kN = 2048;
    std::unique_ptr<std::atomic<float>[]> ring_;
    std::atomic<size_t> wpos_{0};
    dsp::FFT fft_;
    std::vector<float> disp_;
    int w_, h_;
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
        {"sfrecord~", "Samples", "record to a WAV file: [channels]", with<SfrecordBox>()},
        {"waveform~", "Samples", "show a buffer~, select a region: name [w h]", with<WaveformBox>()},
        {"snapshot~", "Analysis", "signal value as a number", [](M a, Patch*) { return std::make_unique<ProbeBox>(false, a); }},
        {"peakamp~", "Analysis", "peak amplitude as a number", [](M a, Patch*) { return std::make_unique<ProbeBox>(true, a); }},
        {"meter~", "Analysis", "level meter", plain<MeterBox>()},
        {"scope~", "Analysis", "oscilloscope", with<ScopeBox>()},
        {"spectroscope~", "Analysis", "live spectrum: [w h]", with<SpectroscopeBox>()},
    });
}

}  // namespace moritz
