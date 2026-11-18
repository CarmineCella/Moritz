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

// the C++ libraries, into an interpreter
inline void musilLibraries(musil::Interp& i) {
    musil::add_std(i);
    musil::add_system(i);
    musil::add_scientific(i);
    musil::add_signals(i);
    musil::add_music(i);
    i.def("synth-render", musilSilentSynth, 3, 4);
}

}  // namespace moritz
