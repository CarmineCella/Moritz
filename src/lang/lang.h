// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// lang.h — the Moritz language: the core and the libraries written in C++. Their halves written in
// the language (std.moritz, signals.moritz, ...) are in language/ and are loaded by name.

#pragma once
#include "core.h"
#include "std.h"
#include "system.h"
#include "scientific.h"
#include "signals.h"
#include "music.h"

namespace moritz::lang {
// the C++ libraries, into an interpreter
inline void make_env(Interp& i) {
    add_std(i);
    add_system(i);
    add_scientific(i);
    add_signals(i);
    add_music(i);
}
}  // namespace moritz::lang
