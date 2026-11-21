// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// multitrack_ops.h — what the multitrack mode does to a session (a score, score_doc.h), without the interface: each
// change of a track's settings, a bus or the session's options is one Edit (one undo step), as timeline_ops.h's; and
// recording into the armed tracks, from the transport's start to its stop, the takes added as clips in one step.
#pragma once

#include "multitrack.h"
#include "score_doc.h"

#include <memory>
#include <string>
#include <vector>

namespace moritz {
namespace mt {

// ---- tracks and buses (each one undo step; false and the document's message when refused)
RowSettings settingsOf(const sc::Score& s, const std::string& track);
bool setSettings(ScoreDoc& d, const std::string& track, const RowSettings& rs, const std::string& what);
std::string addBus(ScoreDoc& d, const std::string& name = "");      // a bus (a row marked so); its name, "" refused
std::vector<std::string> buses(const sc::Score& s);                 // the rows that are buses, in order
bool setSend(ScoreDoc& d, const std::string& track, const std::string& bus, double gain);   // gain <= 0: removed
bool setOutput(ScoreDoc& d, const std::string& track, const std::string& bus);              // "": the master
bool setArm(ScoreDoc& d, const std::string& track, bool on, int input = -1, int channels = -1);   // input from 1
bool setMonitor(ScoreDoc& d, const std::string& track, bool on);
// a track's insert or instrument (a .moritz file; "" none); role "insert" or "instrument"
bool setTrackPatch(ScoreDoc& d, const std::string& track, const std::string& role, const std::string& file);
bool setSession(ScoreDoc& d, const SessionSettings& v, const std::string& what);
// a sound clip's fades (seconds) and shapes (linear, equal, slow, fast) and its gain, one step
bool setClipFades(ScoreDoc& d, uint64_t id, double fadeIn, double fadeOut, const std::string& inShape = "",
                  const std::string& outShape = "");
bool setClipGain(ScoreDoc& d, uint64_t id, double gain);

// ---- recording into the armed tracks of a session
// start(): the takes made (a WAV file each, in the session's media folder: the session must have a file), published to
// the score's player, the transport located before the playhead by the count-in and started; recording from the
// playhead (or the punch range) on. stop(): the transport stopped, the files finished, each take a sound clip on its
// track, moved earlier by the latency (the device's plus the session's offset) so that it lines up with what played,
// all in one undo step ("Record").
class SessionRecorder {
public:
    ~SessionRecorder();
    // root: where the score's own player runs (a box-bound score plays on its box); latency: the device's (samples)
    bool start(const std::shared_ptr<ScoreDoc>& d, Patch* root, double playhead, int deviceLatency, std::string& err);
    bool recording() const { return box_ != nullptr; }
    bool stop(std::string& msg);                // what was added
    std::vector<uint64_t> added;                // the clips the last stop() added
    double startedAt() const { return from_; }  // seconds, where it records from
private:
    std::shared_ptr<ScoreDoc> doc_;
    Box* box_ = nullptr;
    Patch* root_ = nullptr;
    int64_t latency_ = 0;
    double from_ = 0;
};

// ---- rendering: the master in channels, and each track's output (after its strip) as a file of its own, offline
// (score_doc.h's ScoreRender with stems): <wav without .wav>-<track>.wav
std::string stemPath(const std::string& wav, const std::string& track);

}  // namespace mt
}  // namespace moritz
