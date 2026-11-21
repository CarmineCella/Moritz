// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// preview.cpp — the sound editor's preview voice (preview.h)

#include "preview.h"

#include <algorithm>
#include <cmath>

namespace moritz {

PreviewPlayer::~PreviewPlayer() {
    delete slot_.exchange(nullptr);
    delete cur_;
    delete fading_;
    for (Voice*& v : stuck_) delete v;
    collect();
}

uint64_t PreviewPlayer::play(Sound s, long from, long to, bool loop, long start) {
    collect();
    auto* v = new Voice;
    v->sound = std::move(s);
    v->from = std::max(0L, from);
    v->to = to;
    v->start = start >= 0 ? start : v->from;
    v->id = ++lastId_;
    v->pos = (double)v->start;
    v->env = 1.f;
    loop_.store(loop, std::memory_order_relaxed);
    delete slot_.exchange(v, std::memory_order_acq_rel);   // one the audio thread never took
    return v->id;
}

uint64_t PreviewPlayer::playNote(Sound s, long from, long to, double speed, float gain, double fade) {
    collect();
    auto* v = new Voice;
    v->sound = std::move(s);
    v->from = std::max(0L, from);
    v->to = to;
    v->start = v->from;
    v->id = ++lastId_;
    v->pos = (double)v->start;
    v->env = 1.f;
    v->speed = std::isfinite(speed) && speed > 0 ? std::clamp(speed, 1.0 / 64, 64.0) : 1.0;
    v->gain = std::isfinite(gain) ? std::max(0.f, gain) : 1.f;
    v->fade = std::isfinite(fade) ? std::max(0.0, fade) : 0.0;
    loop_.store(false, std::memory_order_relaxed);
    delete slot_.exchange(v, std::memory_order_acq_rel);
    return v->id;
}

void PreviewPlayer::stop() {
    stopBelow_.store(lastId_ + 1, std::memory_order_release);
    delete slot_.exchange(nullptr, std::memory_order_acq_rel);
    collect();
}

bool PreviewPlayer::active(uint64_t id) const {
    if (id == 0 || id < stopBelow_.load(std::memory_order_acquire)) return false;
    if (playing_.load(std::memory_order_acquire) == id) return true;
    Voice* v = slot_.load(std::memory_order_acquire);   // the GUI frees voices, so this one can't go meanwhile
    return v && v->id == id;
}

double PreviewPlayer::position(uint64_t id) const {
    if (playing_.load(std::memory_order_acquire) != id) return -1.0;
    return pos_.load(std::memory_order_relaxed);
}

void PreviewPlayer::collect() {
    Voice* v;
    while (back_.pop(v)) {
        delete v;
        backCount_.fetch_sub(1, std::memory_order_relaxed);
    }
}

int PreviewPlayer::waiting() const { return backCount_.load(std::memory_order_relaxed); }

void PreviewPlayer::giveBack(Voice*& v) {
    if (!v) return;
    if (back_.push(v)) {
        backCount_.fetch_add(1, std::memory_order_relaxed);
    } else {
        for (Voice*& s : stuck_)
            if (!s) { s = v; break; }               // handed back at the next callback
    }
    v = nullptr;
}

// false once the voice is done (its end reached, or faded out)
bool PreviewPlayer::playVoice(Voice& v, float* out, int ch, int frames, double sr, bool loop) {
    const SampleBuffer& b = *v.sound;
    const int bc = b.channels;
    const long end = v.to > v.from ? std::min(v.to, b.frames) : b.frames;
    const long from = std::min(v.from, end);
    if (end - from < 1 || bc < 1 || sr <= 0) return false;
    const double step = (b.sr > 0 ? b.sr / sr : 1.0) * v.speed;
    const double fadeFrames = v.fade * (b.sr > 0 ? b.sr : sr) * v.speed;   // a note's fades, in the sound's frames
    const float down = (float)(1.0 / (0.005 * sr));      // a stop fades out over 5 ms
    const float* x = b.data.data();
    for (int f = 0; f < frames; ++f) {
        if (v.ending) {
            v.env -= down;
            if (v.env <= 0.f) return false;
        }
        if (v.pos >= (double)end) {
            if (!loop || v.ending) return false;
            v.pos = (double)from + std::fmod(v.pos - (double)end, (double)(end - from));
        }
        long i = (long)v.pos;
        float fr = (float)(v.pos - (double)i);
        float g = v.env * v.gain;
        if (fadeFrames > 0) g *= (float)std::clamp(std::min(v.pos - (double)from, (double)end - v.pos) / fadeFrames, 0.0, 1.0);
        auto at = [&](int k) {
            float a = x[(size_t)i * bc + k];
            float n = i + 1 < b.frames ? x[(size_t)(i + 1) * bc + k] : 0.f;
            return a + fr * (n - a);
        };
        float* o = out + (size_t)f * ch;
        if (ch == 1) {
            float s = 0.f;
            for (int k = 0; k < bc; ++k) s += at(k);
            o[0] += g * s / (float)bc;
        } else if (bc == 1) {
            float s = g * at(0);
            o[0] += s;
            o[1] += s;
        } else {
            for (int c = 0; c < std::min(ch, bc); ++c) o[c] += g * at(c);
        }
        v.pos += step;
    }
    return true;
}

bool PreviewPlayer::render(float* out, int ch, int frames, double sr) {
    AudioScope scope;                                    // nothing here allocates or frees (tests check)
    for (Voice*& s : stuck_)
        if (s) giveBack(s);
    if (Voice* v = slot_.exchange(nullptr, std::memory_order_acq_rel)) {
        giveBack(fading_);                               // a third one: the oldest stops at once
        if (cur_) {
            cur_->ending = true;
            fading_ = cur_;
        }
        cur_ = v;
    }
    if (cur_ && cur_->id < stopBelow_.load(std::memory_order_acquire)) cur_->ending = true;
    bool any = cur_ || fading_;
    if (ch > 0 && frames > 0) {
        bool loop = loop_.load(std::memory_order_relaxed);
        if (fading_ && !playVoice(*fading_, out, ch, frames, sr, false)) giveBack(fading_);
        if (cur_ && !playVoice(*cur_, out, ch, frames, sr, loop)) giveBack(cur_);
    }
    pos_.store(cur_ ? cur_->pos : -1.0, std::memory_order_relaxed);
    playing_.store(cur_ && !cur_->ending ? cur_->id : 0, std::memory_order_release);
    return any;
}

}  // namespace moritz
