// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// preview.h — the sound editor's preview: a sound played through the audio engine beside the patches, belonging to
// none of them (AudioEngine adds it to what goes to the device, after the patches; it is metered and recorded too).
//
// The GUI hands the audio thread a voice: a snapshot of the sound (sound_doc.h: samples that never change, shared),
// the frames to play and an id. It waits in a slot until the next callback takes it (the newest wins: one replaced
// in the slot before that was never seen by the audio thread and is freed at once). The audio thread plays it,
// fades it out over a few milliseconds when it is stopped or replaced, and hands it back through a queue when it
// is done with it; collect() frees what came back, on the GUI thread. So the samples stay alive while they are
// played, whatever happens to the editor or the buffer~ they came from, and the audio thread never allocates,
// frees or waits.
#pragma once

#include "sound_doc.h"

#include <atomic>
#include <cstdint>

namespace moritz {

class PreviewPlayer {
public:
    PreviewPlayer() = default;
    PreviewPlayer(const PreviewPlayer&) = delete;
    PreviewPlayer& operator=(const PreviewPlayer&) = delete;
    ~PreviewPlayer();                    // the audio must be stopped

    // ---- GUI thread
    // plays frames [from, to) of s (to <= from: to the end), from frame start (-1: from); its id
    uint64_t play(Sound s, long from, long to, bool loop, long start = -1);
    void stop();                         // whatever plays (faded out)
    void setLoop(bool on) { loop_.store(on, std::memory_order_relaxed); }
    bool loop() const { return loop_.load(std::memory_order_relaxed); }
    // that voice is playing, or waits for the audio thread to take it
    bool active(uint64_t id) const;
    // the frame it is at (-1: not playing yet, or any more)
    double position(uint64_t id) const;
    void collect();                      // frees what the audio thread is done with
    int waiting() const;                 // voices handed back and not freed yet (tests)

    // ---- audio thread: the preview added into out (ch channels interleaved, frames of them) at the device's
    // rate sr; true if it added anything
    bool render(float* out, int ch, int frames, double sr);

private:
    struct Voice {
        Sound sound;
        long from = 0, to = 0, start = 0;
        uint64_t id = 0;
        double pos = 0;                  // audio side: the frame it is at
        float env = 0.f;                 // audio side: 0..1, rises at the start, falls when it stops
        bool ending = false;             // audio side: fading out, then handed back
    };
    void giveBack(Voice*& v);            // audio thread
    static bool playVoice(Voice& v, float* out, int ch, int frames, double sr, bool loop);
    std::atomic<Voice*> slot_{nullptr};
    dsp::Spsc<Voice*, 64> back_;
    Voice* cur_ = nullptr;               // audio side: playing
    Voice* fading_ = nullptr;            // audio side: the one before, fading out
    Voice* stuck_[4] = {};               // audio side: to hand back when the queue had no room (never, in practice)
    uint64_t lastId_ = 0;                // GUI side
    std::atomic<uint64_t> stopBelow_{0}; // voices with an id below this one stop
    std::atomic<uint64_t> playing_{0};   // the id the audio thread plays (0: none)
    std::atomic<double> pos_{-1.0};
    std::atomic<bool> loop_{false};
    std::atomic<int> backCount_{0};
};

}  // namespace moritz
