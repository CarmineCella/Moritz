// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// musil_host.h — Musil, the scripting language of Moritz, as Moritz uses it.
//
// The language is in musil/src, independent of Moritz (tools/check-musil.sh). Moritz takes its core
// and the libraries that compute; it leaves out the live library (Moritz plays) and the plots (Moritz
// draws). The halves written in Musil (std.mu, signals.mu, ...) are found on the load path. What Moritz
// adds or changes is here and in session.cpp, never in musil/src.
#pragma once

#include "core.h"
#include "std.h"
#include "system.h"
#include "scientific.h"
#include "signals.h"
#include "music.h"


#ifdef OPAQUE      // Windows (wingdi.h) defines it; Musil has a value type of that name
#undef OPAQUE
#endif

namespace moritz {

// (synth-render f params seconds [sr]) in Moritz: silence of that length. Musil's synths are its live
//   library, which Moritz leaves out (synths are patches in Moritz); a score's synth events are silent
inline musil::vptr musilSilentSynth(musil::vlist& a, musil::Interp& i) {
    double secs = i.scalar(a[2]), sr = a.size() > 3 ? i.scalar(a[3]) : 44100;
    return musil::v_arr(std::valarray<double>(0.0, (size_t)std::max(0.0, secs * sr)));
}

// the live library's functions (live.h's add_live, without synth-render), which Moritz leaves out
inline constexpr const char* kMusilLive[] = {
    "audio-open", "audio-close", "audio-start", "audio-stop", "audio-status", "audio-devices", "audio-time",
    "play-buffer", "stop", "stop-all", "master-gain", "bus-reverb", "bus-reverb-off", "bus-limiter", "play-cues",
    "stop-cues", "cues-status", "cue-cache-limit", "cue-lookahead", "bus-reverb-latency", "idle-hook!",
    "idle-hook-off!", "idle-hooks", "voice-set", "voices", "synth", "set-param", "free", "synth-params", "synths",
    "streamable", "tempo", "beat", "beat-time", "bpm", "live-loop", "stop-loop", "stop-loops", "loops",
    "lookahead", "control", "toggle", "set-control", "control-value", "controls-list", "bind-control",
    "unbind-control", "clear-controls", "osc-send", "osc-listen", "osc-map", "osc-stop", "osc-encode",
    "osc-decode", "playhead!", "playhead-off!", "playhead"};

// the C++ libraries, into an interpreter
inline void musilLibraries(musil::Interp& i) {
    musil::add_std(i);
    musil::add_system(i);
    musil::add_scientific(i);
    musil::add_signals(i);
    musil::add_music(i);
    i.def("synth-render", musilSilentSynth, 3, 4);
    // the live library's functions stay undefined (scripts ask (defined? "audio-start")), but the error says
    // why: Musil plays live on its own, Moritz plays with patches
    for (const char* name : kMusilLive)
        i.absent[name] = "live audio is Musil's alone: run this script with musil; in Moritz, sound is made by patches";
}

}  // namespace moritz
