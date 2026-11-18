// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// audio.cpp — the miniaudio engine driving Patch::process()

#include "moritz.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

namespace moritz {


AudioEngine::AudioEngine(Patch& p) : patch_(p) {
    // the bus wider than the device (and the monitor's input): allocated once, here, never while
    // the callback may be reading it
    scratch_.assign((size_t)kScratchFrames * kMaxBus, 0.f);
    contextOk_ = ma_context_init(nullptr, 0, nullptr, &context_) == MA_SUCCESS;
    patch_.ioBufferFrames = settings.bufferFrames;
    refreshDevices();
}

AudioEngine::~AudioEngine() {
    close();
    if (contextOk_) ma_context_uninit(&context_);
}

void AudioEngine::refreshDevices() {
    outputs.clear();
    inputs.clear();
    if (!contextOk_) return;
    ma_device_info *pb = nullptr, *cap = nullptr;
    ma_uint32 npb = 0, ncap = 0;
    if (ma_context_get_devices(&context_, &pb, &npb, &cap, &ncap) != MA_SUCCESS) return;
    for (ma_uint32 i = 0; i < npb; ++i) outputs.push_back({pb[i].name, pb[i].id});
    for (ma_uint32 i = 0; i < ncap; ++i) inputs.push_back({cap[i].name, cap[i].id});
}

void AudioEngine::callback(ma_device* d, void* out, const void* in, ma_uint32 frames) {
    static thread_local bool ftz = (dsp::flushDenormals(), true);
    (void)ftz;
    auto* self = static_cast<AudioEngine*>(d->pUserData);
    auto t0 = std::chrono::steady_clock::now();
    int devOut = (int)d->playback.channels, bus = self->bus_.load(std::memory_order_acquire);
    Monitor* mon = self->monitor_.load(std::memory_order_acquire);
    bool testing = self->testChannel.load(std::memory_order_relaxed) >= 0;
    float* dst = static_cast<float*>(out);
    const float* o = dst;
    int ch = devOut;
    if ((bus > devOut || mon) && (size_t)frames * (size_t)bus <= self->scratch_.size()) {
        // the patch's own channels (more than the device's, or monitored):
        // all of them go to the meter and the recorder; the device gets its
        // own channels, or the binaural monitor on 1-2
        float* sc = self->scratch_.data();
        self->patch_.process(static_cast<const float*>(in), (int)d->capture.channels, sc, bus, (int)frames);
        if (testing) self->addTest(sc, bus, (int)frames, d->sampleRate);
        if (mon) {
            self->renderMonitor(*mon, sc, bus, dst, devOut, (int)frames);
        } else {
            for (ma_uint32 f = 0; f < frames; ++f)
                for (int c = 0; c < devOut; ++c) dst[(size_t)f * devOut + c] = sc[(size_t)f * bus + c];
        }
        o = sc;
        ch = bus;
    } else {
        self->patch_.process(static_cast<const float*>(in), (int)d->capture.channels, dst, devOut, (int)frames);
        if (testing) self->addTest(dst, devOut, (int)frames, d->sampleRate);
    }
    if (!testing) self->lastTest_ = -1;
    // the output's peaks (for the level meter) and the quick recorder
    for (int c = 0; c < std::min(ch, kMeterChannels); ++c) {
        float pk = 0.f;
        for (ma_uint32 f = 0; f < frames; ++f) pk = std::max(pk, std::fabs(o[(size_t)f * (size_t)ch + (size_t)c]));
        float prev = self->peak_[c].load(std::memory_order_relaxed);
        if (pk > prev) self->peak_[c].store(pk, std::memory_order_relaxed);
    }
    self->recorder.push(o, (int)frames, ch);
    // CPU load: time spent over the time the buffer lasts, smoothed over ~20 buffers
    double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double period = (double)frames / std::max<ma_uint32>(1, d->sampleRate);
    float load = (float)(used / period), c = self->patch_.cpuLoad.load(std::memory_order_relaxed);
    self->patch_.cpuLoad.store(c + 0.05f * (load - c), std::memory_order_relaxed);
}

bool AudioEngine::start(std::string& err) {
    bool ok = open(err);
    patch_.audioChanged();
    return ok;
}

void AudioEngine::stop() {
    if (!running_) return;
    close();
    patch_.audioChanged();
}

bool AudioEngine::open(std::string& err) {
    close();
    if (!contextOk_) {
        err = "could not initialise the audio system";
        return false;
    }
    bool wantIn = patch_.needsInput() && settings.input != -2;
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool duplex = wantIn && attempt == 0;
        ma_device_config cfg =
            ma_device_config_init(duplex ? ma_device_type_duplex : ma_device_type_playback);
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = (ma_uint32)std::clamp(settings.outChannels, 0, kMaxBus);   // 0: all of them
        if (settings.output >= 0 && settings.output < (int)outputs.size())
            cfg.playback.pDeviceID = &outputs[settings.output].id;
        cfg.capture.format = ma_format_f32;
        cfg.capture.channels = (ma_uint32)std::clamp(settings.inChannels, 0, kMaxBus);
        if (settings.input >= 0 && settings.input < (int)inputs.size())
            cfg.capture.pDeviceID = &inputs[settings.input].id;
        cfg.sampleRate = (ma_uint32)std::max(0, settings.sampleRate);
        cfg.periodSizeInFrames = (ma_uint32)std::max(16, settings.bufferFrames);
        cfg.dataCallback = callback;
        cfg.pUserData = this;
        if (ma_device_init(&context_, &cfg, &device_) != MA_SUCCESS) {
            err = duplex ? "could not open audio input; running output only"
                         : "could not open the audio output device";
            if (duplex) continue;
            return false;
        }
        patch_.sampleRate = (double)device_.sampleRate;
        patch_.rateChanged();     // boxes prepared for another rate prepare again, before audio starts
        patch_.ioBufferFrames = (int)device_.playback.internalPeriodSizeInFrames;
        patch_.cpuLoad = 0.f;
        if (ma_device_start(&device_) != MA_SUCCESS) {
            ma_device_uninit(&device_);
            err = "could not start the audio device";
            return false;
        }
        running_ = true;
        duplex_ = duplex;
        patch_.dspRunning = true;
        notedBus_ = 0;
        updateBus();
        if (attempt == 0) err.clear();
        return true;
    }
    return false;
}

void AudioEngine::close() {
    if (!running_) return;
    ma_device_uninit(&device_);
    running_ = false;
    duplex_ = false;
    patch_.dspRunning = false;
}

// bursts of pink-ish noise on the channel under test: 160 ms on, 90 ms off
void AudioEngine::addTest(float* bus, int busCh, int frames, double sr) {
    int c = testChannel.load(std::memory_order_relaxed);
    if (c < 0 || c >= busCh) return;
    if (c != lastTest_) { testClock_ = 0; lastTest_ = c; }
    float g = testLevel.load(std::memory_order_relaxed);
    long on = (long)(0.16 * sr), period = (long)(0.25 * sr);
    for (int f = 0; f < frames; ++f, ++testClock_) {
        noise_ ^= noise_ << 13; noise_ ^= noise_ >> 17; noise_ ^= noise_ << 5;
        float w = (float)((double)noise_ / 4294967296.0 * 2.0 - 1.0);
        pink_[0] = 0.997f * pink_[0] + 0.029591f * w;      // a few poles: roughly pink
        pink_[1] = 0.985f * pink_[1] + 0.032534f * w;
        pink_[2] = 0.950f * pink_[2] + 0.048056f * w;
        float v = (pink_[0] + pink_[1] + pink_[2] + 0.1848f * w) * 2.0f;
        long ph = testClock_ % period;
        float env = ph < on ? std::min(1.f, std::min(ph, on - ph) / (float)(0.005 * sr)) : 0.f;   // 5 ms ramps
        bus[(size_t)f * busCh + c] += g * env * v;
    }
}

// each channel convolved with the head responses of its speaker, summed
void AudioEngine::renderMonitor(Monitor& m, const float* bus, int busCh, float* out, int outCh, int frames) {
    int n = std::min(m.channels, busCh), T = m.taps;
    for (int f = 0; f < frames; ++f) {
        float l = 0.f, r = 0.f;
        for (int c = 0; c < n; ++c) {
            float* h = &m.hist[(size_t)c * 2 * T];
            float x = bus[(size_t)f * busCh + c];
            h[m.pos] = x;
            h[m.pos + T] = x;
            const float* hx = h + m.pos + 1;                 // the last T samples, oldest first
            float a, b;                                      // the filters are kept reversed: last tap first
            dsp::firPair(&m.hl[(size_t)c * T], &m.hr[(size_t)c * T], hx, T, a, b);
            l += a;
            r += b;
        }
        m.pos = m.pos + 1 == T ? 0 : m.pos + 1;
        for (int c = 0; c < outCh; ++c) out[(size_t)f * outCh + c] = 0.f;
        if (outCh >= 2) {
            out[(size_t)f * outCh] = std::clamp(l, -1.f, 1.f);
            out[(size_t)f * outCh + 1] = std::clamp(r, -1.f, 1.f);
        } else if (outCh == 1) {
            out[(size_t)f] = std::clamp(0.5f * (l + r), -1.f, 1.f);
        }
    }
}

void AudioEngine::setMonitor(const std::vector<std::pair<float, float>>& speakers) {
    if (speakers.empty()) { monitor_.store(nullptr, std::memory_order_release); return; }
    double sr = running_ ? (double)device_.sampleRate : 48000.0;
    auto m = std::make_unique<Monitor>();
    m->channels = (int)speakers.size();
    std::vector<float> l, r;
    for (auto& sp : speakers) {
        hrirFor(sr, sp.first, sp.second, l, r);
        std::reverse(l.begin(), l.end());                   // last tap first: the history runs oldest first
        std::reverse(r.begin(), r.end());
        m->taps = (int)l.size();
        m->hl.insert(m->hl.end(), l.begin(), l.end());
        m->hr.insert(m->hr.end(), r.begin(), r.end());
    }
    m->hist.assign((size_t)m->channels * 2 * (size_t)m->taps, 0.f);
    monitor_.store(m.get(), std::memory_order_release);
    monitors_.push_back(std::move(m));
}

void AudioEngine::updateBus() {
    if (!running_) return;
    int dev = (int)device_.playback.channels;
    int used = std::min(patch_.maxOutputChannel(), (int)kMaxBus);
    int bus = std::max(dev, used);
    bus_.store(bus, std::memory_order_release);
    if (bus > dev && bus != notedBus_)
        patch_.print("audio: the patches use " + std::to_string(bus) + " output channels and the device has " +
                     std::to_string(dev) + ": channels " + std::to_string(dev + 1) + "-" + std::to_string(bus) +
                     " are metered and recorded (quick recorder), not heard");
    notedBus_ = bus > dev ? bus : 0;
}

void AudioEngine::updateInputNeed() {
    updateBus();
    if (!running_) return;
    bool want = patch_.needsInput() && settings.input != -2;
    if (want != duplex_) {
        std::string err;
        start(err);
    }
}

}  // namespace moritz
