// audio.cpp — the miniaudio engine driving Patch::process()

#include "minlsp.h"

#include <algorithm>
#include <cstring>

namespace mlsp {


AudioEngine::AudioEngine(Patch& p) : patch_(p) {
    contextOk_ = ma_context_init(nullptr, 0, nullptr, &context_) == MA_SUCCESS;
    refreshDevices();
}

AudioEngine::~AudioEngine() {
    stop();
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
    self->patch_.process(static_cast<const float*>(in), (int)d->capture.channels,
                         static_cast<float*>(out), (int)d->playback.channels, (int)frames);
}

bool AudioEngine::start(std::string& err) {
    stop();
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
        cfg.playback.channels = 2;
        if (settings.output >= 0 && settings.output < (int)outputs.size())
            cfg.playback.pDeviceID = &outputs[settings.output].id;
        cfg.capture.format = ma_format_f32;
        cfg.capture.channels = 2;
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
        if (ma_device_start(&device_) != MA_SUCCESS) {
            ma_device_uninit(&device_);
            err = "could not start the audio device";
            return false;
        }
        running_ = true;
        duplex_ = duplex;
        patch_.dspRunning = true;
        if (attempt == 0) err.clear();
        return true;
    }
    return false;
}

void AudioEngine::stop() {
    if (!running_) return;
    ma_device_uninit(&device_);
    running_ = false;
    duplex_ = false;
    patch_.dspRunning = false;
}

void AudioEngine::updateInputNeed() {
    if (!running_) return;
    bool want = patch_.needsInput() && settings.input != -2;
    if (want != duplex_) {
        std::string err;
        start(err);
    }
}

}  // namespace mlsp
