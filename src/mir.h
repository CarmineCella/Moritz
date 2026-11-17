// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// mir.h — the analyses of Moritz's objects, offline (spectrogram, melgram, mfccgram, chromagram, onsets,
// beats, keyfind, similarity, novelty, curves) and live (melbands~, mfcc~, chroma~, onset~): Musil's
// (musil/src/signals/mir.h), so that a patch and a script analyse a sound the same way.
#pragma once

#include "signals/mir.h"

namespace moritz {
namespace mir = musil::mir;
}  // namespace moritz
